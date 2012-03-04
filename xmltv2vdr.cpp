/*
 * xmltv2vdr.cpp: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <vdr/plugin.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <pwd.h>
#include "xmltv2vdr.h"
#include "parse.h"
#include "extpipe.h"
#include "setup.h"

#if __GNUC__ > 3
#define UNUSED(v) UNUSED_ ## v __attribute__((unused))
#else
#define UNUSED(x) x
#endif

// -------------------------------------------------------------

cEPGChannel::cEPGChannel(const char *Name, bool InUse)
{
    name=strdup(Name);
    inuse=InUse;
}

cEPGChannel::~cEPGChannel()
{
    if (name) free((void *) name);
}

int cEPGChannel::Compare(const cListObject &ListObject) const
{
    cEPGChannel *epgchannel= (cEPGChannel *) &ListObject;
    return strcmp(name,epgchannel->Name());
}

// -------------------------------------------------------------

cEPGExecutor::cEPGExecutor(cEPGSources *Sources) : cThread("xmltv2vdr importer")
{
    sources=Sources;
    textmappings=NULL;
    epall=false;
}

void cEPGExecutor::Action()
{
    if (!sources) return;
    int ret=0;
    for (cEPGSource *epgs=sources->First(); epgs; epgs=sources->Next(epgs))
    {
        int retries=0;
        while (retries<=2)
        {
            ret=epgs->Execute(*this);
            if ((ret>0) && (ret<126) && (retries<2))
            {
                epgs->Dlog("waiting 60 seconds");
                int l=0;
                while (l<300)
                {
                    struct timespec req;
                    req.tv_sec=0;
                    req.tv_nsec=200000000; // 200ms
                    nanosleep(&req,NULL);
                    if (!Running())
                    {
                        epgs->Ilog("request to stop from vdr");
                        return;
                    }
                    l++;
                }
                retries++;
            }
            else
            {
                break;
            }
        }
        if (retries>=2) epgs->Elog("skipping after %i retries",retries);
        if (!ret) break; // TODO: check if we must execute second/third source!
    }
    if (!ret && epall && textmappings)
    {
        struct passwd pwd,*pwdbuf;
        char buf[1024];
        getpwuid_r(getuid(),&pwd,buf,sizeof(buf),&pwdbuf);
        if (pwdbuf)
        {
            char *epdir;
            if (asprintf(&epdir,"%s/.eplists/lists",pwdbuf->pw_dir)!=-1)
            {
                if (!access(epdir,R_OK))
                {
                    int retries=0;
                    while (retries<=2)
                    {

                        if (!cParse::AddSeasonEpisode2TimerChannels(epdir,textmappings))
                        {
                            dsyslog("waiting 60 seconds");
                            retries++;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                free(epdir);
            }
        }
    }
    if (!ret) cSchedules::Cleanup(true);
}



// -------------------------------------------------------------

cEPGSource::cEPGSource(const char *Name, const char *ConfDir, cEPGMappings *Maps, cTEXTMappings *Texts)
{
    dsyslog("xmltv2vdr: '%s' added epgsource",Name);
    name=strdup(Name);
    confdir=strdup(ConfDir);
    pin=NULL;
    Log=NULL;
    loglen=0;
    usepipe=false;
    needpin=false;
    running=false;
    daysinadvance=0;
    lastexec=(time_t) 0;
    ready2parse=ReadConfig();
    parse=new cParse(this, Maps, Texts);
    Dlog("is%sready2parse",(ready2parse && parse) ? " " : " not ");
}

cEPGSource::~cEPGSource()
{
    dsyslog("xmltv2vdr: '%s' epgsource removed",name);
    free((void *) name);
    free((void *) confdir);
    if (pin) free((void *) pin);
    if (Log) free((void *) Log);
    if (parse) delete parse;
}

bool cEPGSource::ReadConfig()
{
    char *fname=NULL;
    if (asprintf(&fname,"%s/%s",EPGSOURCES,name)==-1)
    {
        Elog("out of memory");
        return false;
    }
    FILE *f=fopen(fname,"r");
    if (!f)
    {
        Elog("cannot read config file %s",fname);
        free(fname);
        return false;
    }
    Dlog("reading source config");
    size_t lsize;
    char *line=NULL;
    int linenr=1;
    while (getline(&line,&lsize,f)!=-1)
    {
        if (linenr==1)
        {
            if (!strncmp(line,"pipe",4))
            {
                Dlog("is providing data through a pipe");
                usepipe=true;
            }
            else
            {
                Dlog("is providing data through a file");
                usepipe=false;
            }
            char *ndt=strchr(line,';');
            if (ndt)
            {
                *ndt=0;
                ndt++;
                char *pn=strchr(ndt,';');
                if (pn)
                {
                    *pn=0;
                    pn++;
                }
                /*
                  newdatatime=atoi(ndt);
                  if (!newdatatime) Dlog("updates source data @%02i:%02i",1,2);
                */
                if (pn)
                {
                    pn=compactspace(pn);
                    if (pn[0]=='1')
                    {
                        Dlog("is needing a pin");
                        needpin=true;
                    }
                }
            }
        }
        if (linenr==2)
        {
            char *semicolon=strchr(line,';');
            if (semicolon)
            {
                // backward compatibility
                *semicolon=0;
                semicolon++;
                daysmax=atoi(semicolon);
            }
            else
            {
                daysmax=atoi(line);
            }
            Dlog("daysmax=%i",daysmax);
        }
        if (linenr>2)
        {
            // channels
            char *semicolon=strchr(line,';');
            if (semicolon) *semicolon=0;
            char *lf=strchr(line,10);
            if (lf) *lf=0;
            char *cname=line;
            if (line[0]=='*')
            {
                // backward compatibility
                cname++;
            }
            if (!strchr(cname,' ') && (strlen(cname)>0))
            {
                cEPGChannel *epgchannel= new cEPGChannel(cname,false);
                if (epgchannel) channels.Add(epgchannel);
            }
        }
        linenr++;
    }
    if (line) free(line);
    channels.Sort();
    fclose(f);
    free(fname);

    /* --------------- */

    if (asprintf(&fname,"%s/%s",confdir,name)==-1)
    {
        Elog("out of memory");
        return false;
    }
    f=fopen(fname,"r+");
    if (!f)
    {
        if (errno!=ENOENT)
        {
            Elog("cannot read config file %s",fname);
            free(fname);
            return true;
        }
        /* still no config? -> ok */
        free(fname);
        return true;
    }
    Dlog("reading plugin config");
    line=NULL;
    linenr=1;
    while (getline(&line,&lsize,f)!=-1)
    {
        if ((linenr==1) && (needpin))
        {
            char *lf=strchr(line,10);
            if (lf) *lf=0;
            if (strcmp(line,"#no pin"))
            {
                ChangePin(line);
                Dlog("pin set");
            }
        }
        if (linenr==2)
        {
            daysinadvance=atoi(line);
            Dlog("daysinadvance=%i",daysinadvance);
        }
        if (linenr>2)
        {
            // channels
            char *lf=strchr(line,10);
            if (lf) *lf=0;

            for (int x=0; x<channels.Count(); x++)
            {
                if (!strcmp(line,channels.Get(x)->Name()))
                {
                    channels.Get(x)->SetUsage(true);
                    break;
                }
            }
        }
        linenr++;
    }
    if (line) free(line);
    channels.Sort();
    fclose(f);
    free(fname);

    return true;
}

int cEPGSource::ReadOutput(char *&result, size_t &l)
{
    int ret=0;
    char *fname=NULL;
    if (asprintf(&fname,"%s/%s.xmltv",EPGSOURCES,name)==-1)
    {
        Elog("out of memory");
        return 134;
    }
    Dlog("reading from '%s'",fname);

    int fd=open(fname,O_RDONLY);
    if (fd==-1)
    {
        Elog("failed to open '%s'",fname);
        free(fname);
        return 157;
    }

    struct stat statbuf;
    if (fstat(fd,&statbuf)==-1)
    {
        Elog("failed to stat '%s'",fname);
        close(fd);
        free(fname);
        return 157;
    }
    l=statbuf.st_size;
    result=(char *) malloc(l+1);
    if (!result)
    {
        close(fd);
        free(fname);
        Elog("out of memory");
        return 134;
    }
    if (read(fd,result,statbuf.st_size)!=statbuf.st_size)
    {
        Elog("failed to read '%s'",fname);
        ret=149;
        free(result);
        result=NULL;
    }
    close(fd);
    free(fname);
    return ret;
}

int cEPGSource::Execute(cEPGExecutor &myExecutor)
{
    if (!ready2parse) return false;
    if (!parse) return false;
    char *r_out=NULL;
    char *r_err=NULL;
    int l_out=0;
    int l_err=0;
    int ret=0;

    if ((Log) && (lastexec))
    {
        free(Log);
        Log=NULL;
        loglen=0;
    }

    char *cmd=NULL;
    if (asprintf(&cmd,"%s %i '%s'",name,daysinadvance,pin ? pin : "")==-1)
    {
        Elog("out of memory");
        return 134;
    }

    for (int x=0; x<channels.Count(); x++)
    {
        if (channels.Get(x)->InUse())
        {
            int len=strlen(cmd);
            int clen=strlen(channels.Get(x)->Name());
            char *ncmd=(char *) realloc(cmd,len+clen+5);
            if (!ncmd)
            {
                free(cmd);
                Elog("out of memory");
                return 134;
            }
            cmd=ncmd;
            strcat(cmd," ");
            strcat(cmd,channels.Get(x)->Name());
            strcat(cmd," ");
        }
    }
    char *pcmd=strdup(cmd);
    if (pcmd)
    {
        char *pa=strchr(pcmd,'\'');
        char *pe=strchr(pa+1,'\'');
        if (pa && pe)
        {
            pa++;
            for (char *c=pa; c<pe; c++)
            {
                if (c==pa)
                {
                    *c='X';
                }
                else
                {
                    *c='@';
                }
            }
            pe=pcmd;
            while (*pe)
            {
                if (*pe=='@')
                {
                    memmove(pe,pe+1,strlen(pe));
                }
                else
                {
                    pe++;
                }
            }
            Ilog("%s",pcmd);
        }
        free(pcmd);
    }
    cExtPipe p;
    if (!p.Open(cmd))
    {
        free(cmd);
        Elog("failed to open pipe");
        return 141;
    }
    free(cmd);
    Dlog("executing epgsource");
    running=true;

    int fdsopen=2;
    while (fdsopen>0)
    {
        struct pollfd fds[2];
        fds[0].fd=p.Out();
        fds[0].events=POLLIN;
        fds[1].fd=p.Err();
        fds[1].events=POLLIN;
        if (poll(fds,2,500)>=0)
        {
            if (fds[0].revents & POLLIN)
            {
                int n;
                if (ioctl(p.Out(),FIONREAD,&n)<0)
                {
                    n=1;
                }
                r_out=(char *) realloc(r_out, l_out+n+1);
                int l=read(p.Out(),r_out+l_out,n);
                if (l>0)
                {
                    l_out+=l;
                }
            }
            if (fds[1].revents & POLLIN)
            {
                int n;
                if (ioctl(p.Err(),FIONREAD,&n)<0)
                {
                    n=1;
                }
                r_err=(char *) realloc(r_err, l_err+n+1);
                int l=read(p.Err(),r_err+l_err,n);
                if (l>0)
                {
                    l_err+=l;
                }
            }
            if (fds[0].revents & POLLHUP)
            {
                fdsopen--;
            }
            if (fds[1].revents & POLLHUP)
            {
                fdsopen--;
            }
            if (!myExecutor.StillRunning())
            {
                int status;
                p.Close(status);
                if (r_out) free(r_out);
                if (r_err) free(r_err);
                Ilog("request to stop from vdr");
                running=false;
                return 0;
            }
        }
        else
        {
            Elog("failed polling");
            break;
        }
    }
    if (r_out) r_out[l_out]=0;
    if (r_err) r_err[l_err]=0;

    if (usepipe)
    {
        int status;
        if (p.Close(status)>0)
        {
            int returncode=WEXITSTATUS(status);
            if ((!returncode) && (r_out))
            {
                //Dlog("xmltv2vdr: '%s' parsing output");
                Dlog("parsing output");
                ret=parse->Process(myExecutor,r_out,l_out);
            }
            else
            {
                Elog("epgsource returned %i",returncode);
                ret=returncode;
            }
        }
        else
        {
            Elog("failed to execute");
            ret=126;
        }
    }
    else
    {
        int status;
        if (p.Close(status)>0)
        {
            int returncode=WEXITSTATUS(status);
            if (!returncode)
            {
                size_t l;
                char *result=NULL;
                ret=ReadOutput(result,l);
                if ((!ret) && (result))
                {
                    ret=parse->Process(myExecutor,result,l);
                }
                if (result) free(result);
            }
            else
            {
                Elog("epgsource returned %i",returncode);
                ret=returncode;
            }
        }
    }
    if (r_out) free(r_out);
    if (!ret) lastexec=time(NULL);
    if (r_err)
    {
        char *saveptr;
        char *pch=strtok_r(r_err,"\n",&saveptr);
        char *last=(char *) "";
        while (pch)
        {
            if (strcmp(last,pch))
            {
                Elog("%s",pch);
                last=pch;
            }
            pch=strtok_r(NULL,"\n",&saveptr);
        }
        free(r_err);
    }
    running=false;
    return ret;
}

void cEPGSource::ChangeChannelSelection(int *Selection)
{
    for (int i=0; i<channels.Count(); i++)
    {
        channels.Get(i)->SetUsage(Selection[i]);
    }
}

void cEPGSource::Store(void)
{
    char *fname1=NULL;
    char *fname2=NULL;
    if (asprintf(&fname1,"%s/%s",confdir,name)==-1) return;
    if (asprintf(&fname2,"%s/%s.new",confdir,name)==-1)
    {
        Elog("out of memory");
        free(fname1);
        return;
    }

    FILE *w=fopen(fname2,"w+");
    if (!w)
    {
        Elog("cannot create %s",fname2);
        unlink(fname2);
        free(fname1);
        free(fname2);
        return;
    }

    if (pin)
    {
        fprintf(w,"%s\n",pin);
    }
    else
    {
        fprintf(w,"#no pin\n");
    }
    fprintf(w,"%i\n",DaysInAdvance());
    for (int i=0; i<ChannelList()->Count(); i++)
    {
        if (ChannelList()->Get(i)->InUse())
        {
            fprintf(w,"%s\n",ChannelList()->Get(i)->Name());
        }
    }
    fclose(w);

    struct stat statbuf;
    if (stat(confdir,&statbuf)!=-1)
    {
        if (chown(fname2,statbuf.st_uid,statbuf.st_gid)) {}
    }

    rename(fname2,fname1);
    free(fname1);
    free(fname2);
}

void cEPGSource::add2Log(const char Prefix, const char *line)
{
    if (!line) return;

    struct tm tm;
    time_t now=time(NULL);
    localtime_r(&now,&tm);
    char dt[30];
    strftime(dt,sizeof(dt)-1,"%H:%M ",&tm);

    loglen+=strlen(line)+3+strlen(dt);
    char *nptr=(char *) realloc(Log,loglen);
    if (nptr)
    {
        if (!Log) nptr[0]=0;
        Log=nptr;
        char prefix[2];
        prefix[0]=Prefix;
        prefix[1]=0;
        strcat(Log,prefix);
        strcat(Log,dt);
        strcat(Log,line);
        strcat(Log,"\n");
        Log[loglen-1]=0;
    }
}

void cEPGSource::Elog(const char *format, ...)
{
    va_list ap;
    char fmt[255];
    if (snprintf(fmt,sizeof(fmt),"xmltv2vdr '%s' ERROR %s",name,format)==-1) return;
    va_start(ap, format);
    char *ptr;
    if (vasprintf(&ptr,fmt,ap)==-1) return;
    va_end(ap);
    esyslog(ptr);
    add2Log('E',ptr+19+strlen(name));
    free(ptr);
}

void cEPGSource::Dlog(const char *format, ...)
{
    va_list ap;
    char fmt[255];
    if (snprintf(fmt,sizeof(fmt),"xmltv2vdr '%s' %s",name,format)==-1) return;
    va_start(ap, format);
    char *ptr;
    if (vasprintf(&ptr,fmt,ap)==-1) return;
    va_end(ap);
    dsyslog(ptr);
    add2Log('D',ptr+13+strlen(name));
    free(ptr);
}

void cEPGSource::Ilog(const char *format, ...)
{
    va_list ap;
    char fmt[255];
    if (snprintf(fmt,sizeof(fmt),"xmltv2vdr '%s' %s",name,format)==-1) return;
    va_start(ap, format);
    char *ptr;
    if (vasprintf(&ptr,fmt,ap)==-1) return;
    va_end(ap);
    isyslog(ptr);
    add2Log('I',ptr+13+strlen(name));
    free(ptr);
}

// -------------------------------------------------------------

bool cPluginXmltv2vdr::epgsourceexists(const char *name)
{
    if (!epgsources.Count()) return false;
    for (cEPGSource *epgs=epgsources.First(); epgs; epgs=epgsources.Next(epgs))
    {
        if (!strcmp(epgs->Name(),name)) return true;
    }
    return false;
}

void cPluginXmltv2vdr::removeepgmappings()
{
    cEPGMapping *maps;
    while ((maps=epgmappings.Last())!=NULL)
    {
        epgmappings.Del(maps);
    }
}

void cPluginXmltv2vdr::removetextmappings()
{
    cTEXTMapping *maps;
    while ((maps=textmappings.Last())!=NULL)
    {
        textmappings.Del(maps);
    }
}

void cPluginXmltv2vdr::removeepgsources()
{
    cEPGSource *epgs;
    while ((epgs=epgsources.Last())!=NULL)
    {
        epgsources.Del(epgs);
    }
}

cEPGMapping *cPluginXmltv2vdr::EPGMapping(const char *ChannelName)
{
    if (!ChannelName) return NULL;
    if (!epgmappings.Count()) return NULL;
    for (cEPGMapping *maps=epgmappings.First(); maps; maps=epgmappings.Next(maps))
    {
        if (!strcmp(maps->ChannelName(),ChannelName)) return maps;
    }
    return NULL;
}

cTEXTMapping *cPluginXmltv2vdr::TEXTMapping(const char *Name)
{
    if (!textmappings.Count()) return NULL;
    for (cTEXTMapping *textmap=textmappings.First(); textmap; textmap=textmappings.Next(textmap))
    {
        if (!strcmp(textmap->Name(),Name)) return textmap;
    }
    return NULL;
}

void cPluginXmltv2vdr::ReadInEPGSources(bool Reload)
{
    if (Reload) removeepgsources();
    DIR *dir=opendir(EPGSOURCES);
    if (!dir) return;
    struct dirent *dirent;
    while (dirent=readdir(dir))
    {
        if (strchr(&dirent->d_name[0],'.')) continue;
        if (!epgsourceexists(dirent->d_name))
        {
            char *path=NULL;
            if (asprintf(&path,"%s/%s",EPGSOURCES,dirent->d_name)!=-1)
            {
                if (access(path,R_OK)!=-1)
                {
                    int fd=open(path,O_RDONLY);
                    if (fd!=-1)
                    {
                        char id[5];
                        if (read(fd,id,4)!=4)
                        {
                            esyslog("xmltv2vdr: cannot read config file '%s'",dirent->d_name);
                        }
                        else
                        {
                            id[4]=0;
                            if (!strcmp(id,"file") || !strcmp(id,"pipe"))
                            {
                                epgsources.Add(new cEPGSource(dirent->d_name,confdir,&epgmappings,&textmappings));
                            }
                            else
                            {
                                dsyslog("xmltv2vdr: ignoring non config file '%s'",dirent->d_name);
                            }
                            close(fd);
                        }
                    }
                    else
                    {
                        esyslog("xmltv2vdr: cannot open config file '%s'",dirent->d_name);
                    }
                }
                else
                {
                    esyslog("xmltv2vdr: cannot access config file '%s'",dirent->d_name);
                }
                free(path);
            }
        }
    }
    closedir(dir);
}

void cPluginXmltv2vdr::SetExecTime(int ExecTime)
{
    exectime=ExecTime;
    exectime_t=cTimer::SetTime(time(NULL),cTimer::TimeToInt(exectime));
    if (exectime_t<=time(NULL)) exectime_t+=86000;
}

cPluginXmltv2vdr::cPluginXmltv2vdr(void) : epgexecutor(&epgsources)
{
    // Initialize any member variables here.
    // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
    // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
    confdir=NULL;
    WakeUp=0;
    UpStart=1;
    last_exectime_t=0;
    exectime=200;
    SetEPAll(false);
    SetExecTime(exectime);
    TEXTMappingAdd(new cTEXTMapping("country",tr("country")));
    TEXTMappingAdd(new cTEXTMapping("date",tr("year")));
    TEXTMappingAdd(new cTEXTMapping("originaltitle",tr("originaltitle")));
    TEXTMappingAdd(new cTEXTMapping("category",tr("category")));
    TEXTMappingAdd(new cTEXTMapping("actor",tr("actor")));
    TEXTMappingAdd(new cTEXTMapping("adapter",tr("adapter")));
    TEXTMappingAdd(new cTEXTMapping("commentator",tr("commentator")));
    TEXTMappingAdd(new cTEXTMapping("composer",tr("composer")));
    TEXTMappingAdd(new cTEXTMapping("director",tr("director")));
    TEXTMappingAdd(new cTEXTMapping("editor",tr("editor")));
    TEXTMappingAdd(new cTEXTMapping("guest",tr("guest")));
    TEXTMappingAdd(new cTEXTMapping("presenter",tr("presenter")));
    TEXTMappingAdd(new cTEXTMapping("producer",tr("producer")));
    TEXTMappingAdd(new cTEXTMapping("writer",tr("writer")));
    TEXTMappingAdd(new cTEXTMapping("review",tr("review")));
    TEXTMappingAdd(new cTEXTMapping("season",tr("season")));
    TEXTMappingAdd(new cTEXTMapping("episode",tr("episode")));
}

cPluginXmltv2vdr::~cPluginXmltv2vdr()
{
    // Clean up after yourself!
}

const char *cPluginXmltv2vdr::CommandLineHelp(void)
{
    // Return a string that describes all known command line options.
    return NULL;
}

bool cPluginXmltv2vdr::ProcessArgs(int UNUSED(argc), char *UNUSED(argv[]))
{
    // Implement command line argument processing here if applicable.
    return true;
}

bool cPluginXmltv2vdr::Initialize(void)
{
    // Initialize any background activities the plugin shall perform.
    return true;
}

bool cPluginXmltv2vdr::Start(void)
{
    // Start any background activities the plugin shall perform.
    confdir=strdup(ConfigDirectory(PLUGIN_NAME_I18N)); // creates internally the confdir!
    cParse::InitLibXML();
    ReadInEPGSources();
    if (UpStart)
    {
        exectime_t=time(NULL)+60;
        struct tm tm;
        localtime_r(&exectime_t,&tm);
        // prevent from getting startet again
        exectime=tm.tm_hour*100+tm.tm_min;
    }
    else
    {
        if (!exectime_t)
        {
            exectime_t=time(NULL)-60;
            last_exectime_t=exectime_t;
        }
    }
    return true;
}

void cPluginXmltv2vdr::Stop(void)
{
    // Stop any background activities the plugin is performing.
    epgexecutor.Stop();
    removeepgsources();
    removeepgmappings();
    removetextmappings();
    cParse::CleanupLibXML();
    if (confdir)
    {
        free(confdir);
        confdir=NULL;
    }
}

void cPluginXmltv2vdr::Housekeeping(void)
{
    // Perform any cleanup or other regular tasks.
}

void cPluginXmltv2vdr::MainThreadHook(void)
{
    // Perform actions in the context of the main program thread.
    // WARNING: Use with great care - see PLUGINS.html!
    time_t now=time(NULL);
    if (((now>=exectime_t) && (now<(exectime_t+10))) &&
            (last_exectime_t!=exectime_t))
    {
        epgexecutor.Start();
        last_exectime_t=exectime_t;
        SetExecTime(exectime);
    }
}

cString cPluginXmltv2vdr::Active(void)
{
    // Return a message string if shutdown should be postponed
    if (epgexecutor.Active())
    {
        return tr("xmltv2vdr plugin still working");
    }
    return NULL;
}

time_t cPluginXmltv2vdr::WakeupTime(void)
{
    // Return custom wakeup time for shutdown script
    if (WakeUp)
    {
        time_t Now=time(NULL);
        time_t Time=cTimer::SetTime(Now,cTimer::TimeToInt(exectime));
        Time-=180;
        if (Time<=Now)
            Time=cTimer::IncDay(Time,1);
        return Time;
    }
    else
    {
        return 0;
    }
}

const char *cPluginXmltv2vdr::MainMenuEntry(void)
{
    // Return a main menu entry
    return NULL;
}

cOsdObject *cPluginXmltv2vdr::MainMenuAction(void)
{
    // Perform the action when selected from the main VDR menu.
    return NULL;
}

cMenuSetupPage *cPluginXmltv2vdr::SetupMenu(void)
{
    // Return a setup menu in case the plugin supports one.
    return new cMenuSetupXmltv2vdr(this);
}

bool cPluginXmltv2vdr::SetupParse(const char *Name, const char *Value)
{
    // Parse your own setup parameters and store their values.
    if (!strncasecmp(Name,"channel",7))
    {
        if (strlen(Name)<10) return false;

        cEPGMapping *map = new cEPGMapping(&Name[8],Value);
        epgmappings.Add(map);
    }
    else if (!strncasecmp(Name,"textmap",7))
    {
        if (strlen(Name)<10) return false;
        cTEXTMapping *textmap=TEXTMapping(&Name[8]);
        if (textmap)
        {
            textmap->ChangeValue(Value);
        }
        else
        {
            cTEXTMapping *textmap = new cTEXTMapping(&Name[8],Value);
            textmappings.Add(textmap);
        }
    }
    else if (!strcasecmp(Name,"options.exectime"))
    {
        SetExecTime(atoi(Value));
    }
    else if (!strcasecmp(Name,"options.wakeup"))
    {
        WakeUp=atoi(Value);
    }
    else if (!strcasecmp(Name,"options.upstart"))
    {
        UpStart=atoi(Value);
    }
    else if (!strcasecmp(Name,"options.epall"))
    {
        SetEPAll((bool) atoi(Value));
    }
    else return false;
    return true;
}

bool cPluginXmltv2vdr::Service(const char *UNUSED(Id), void *UNUSED(Data))
{
    // Handle custom service requests from other plugins
    return false;
}

const char **cPluginXmltv2vdr::SVDRPHelpPages(void)
{
    // Returns help text
    static const char *HelpPages[]=
    {
        "UPDT\n"
        "    Start epg update",
        NULL
    };
    return HelpPages;
}

cString cPluginXmltv2vdr::SVDRPCommand(const char *Command, const char *UNUSED(Option), int &ReplyCode)
{
    // Process SVDRP commands

    cString output;
    if (!strcasecmp(Command,"UPDT"))
    {
        if (!epgsources.Count())
        {
            ReplyCode=550;
            output="No epg sources installed\n";
        }
        else
        {
            if (epgexecutor.Start())
            {
                ReplyCode=250;
                output="Update started\n";
            }
            else
            {
                ReplyCode=550;
                output="Update already running\n";
            }
        }
    }
    else
    {
        return NULL;
    }
    return output;
}

VDRPLUGINCREATOR(cPluginXmltv2vdr) // Don't touch this!
