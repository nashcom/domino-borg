/*
###########################################################################
# Domino Borg Backup Integration                                          #
# Version 0.9.3 30.03.2024                                                #
# (C) Copyright Daniel Nashed/NashCom 2023-2024                           #
#                                                                         #
# Licensed under the Apache License, Version 2.0 (the "License");         #
# you may not use this file except in compliance with the License.        #
# You may obtain a copy of the License at                                 #
#                                                                         #
#      http://www.apache.org/licenses/LICENSE-2.0                         #
#                                                                         #
# Unless required by applicable law or agreed to in writing, software     #
# distributed under the License is distributed on an "AS IS" BASIS,       #
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.#
# See the License for the specific language governing permissions and     #
# limitations under the License.                                          #
#                                                                         #
#                                                                         #
###########################################################################
*/

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>

#define MAX_BUFFER 1048576 /* 1 MB */
#define MAX_PATH      2048

#define READ 0
#define WRITE 1

/* Global buffer used for all I/O */
unsigned char  g_Buffer[MAX_BUFFER+1] = {0};

char  g_szVersion[]          = "0.9.3";
char  g_szBackupEndMarker[]  = "::BORG-BACKUP-END::";
char  g_szSSH_AUTH_SOCK[]    = "SSH_AUTH_SOCK";
char  g_szSSH_AGENT_PID[]    = "SSH_AGENT_PID";

char  g_szBorgBackupBinary[MAX_PATH+1] = "/usr/bin/borg";
char  g_szSSHAgentBinary[MAX_PATH+1]   = "/usr/bin/ssh-agent";
char  g_szSSHAddBinary[MAX_PATH+1]     = "/usr/bin/ssh-add";
char  g_szTarBinary[MAX_PATH+1]        = "/usr/bin/tar";
char  g_szBorgRepo[MAX_PATH+1]         = "/local/backup/borg";
char  g_nshBorgDir[1024+1]             = {0};
char  g_szFilePID[MAX_PATH+1]          = {0};
char  g_szBorgLogFile[MAX_PATH+1]      = {0};
char  g_szPassCommand[MAX_PATH+1]      = {0};
char  g_szBorgRSH[MAX_PATH+1]          = {0};
char  g_szBaseDir[MAX_PATH+1]          = {0};
char  g_szRemotePath[MAX_PATH+1]       = {0};
char  g_szPassphrase[256]              = {0};
char  g_szSSHAuthSock[MAX_PATH+1]      = {0};
char  g_szSSHKeyFile[MAX_PATH+1]       = {0};
char  g_szSSHKey[8000]                 = {0};

pid_t g_SSHAgentPID       =   0;
int   g_SSHKeyLife        =  20;
int   g_WaitTime          = 500;
int   g_Verbose           =   0;
int   g_BorgDeleteAllowed =   0;
long  g_MinPruneDays      =   7;

uid_t g_uid  = getuid();
gid_t g_gid  = getgid();
uid_t g_euid = geteuid();
gid_t g_egid = getegid();

void PrintUser (const char *pszHeader, uid_t uid)
{
    struct passwd *pPasswd = NULL;

    pPasswd = getpwuid (uid);

    if (NULL == pszHeader)
        return;

    if (NULL == pPasswd)
        return;

    if (pPasswd->pw_name)
        printf ("%s: %s (%d)\n", pszHeader, pPasswd->pw_name, uid);
}

void PrintGroup (const char *pszHeader, gid_t gid)
{
    struct group *pGroup = NULL;

    if (NULL == pszHeader)
        return;

    pGroup = getgrgid (gid);

    if (NULL == pGroup)
        return;

    if (pGroup->gr_name)
        printf ("%s: %s (%d)\n", pszHeader, pGroup->gr_name, gid);
}


void DumpUser (const char *pszHeader)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    uid_t euid = geteuid();
    gid_t egid = getegid();

    if (pszHeader && *pszHeader)
    {
        printf ("--- %s ---\n", pszHeader);
    }

    PrintUser  (" uid", uid);
    PrintUser  ("euid", euid);
    PrintGroup (" gid", gid);
    PrintGroup ("egid", egid);

    printf ("\n");
}

void DumpEnvironment (const char *pszHeader)
{
    char **e = environ;

    if (pszHeader && *pszHeader)
    {
        printf ("--- %s ---\n", pszHeader);
    }

    while (*e)
    {
        printf ("%s\n", *e);
        e++;
    }

    printf ("\n");
}


int SwitchUser (uid_t new_uid, gid_t new_gid, bool bSetEnv)
{
    uid_t uid  = getuid();
    gid_t gid  = getgid();
    uid_t euid = geteuid();
    gid_t egid = getegid();

    int upd = 0;

    struct passwd *pPasswd = NULL;

    if ((new_gid != gid) || (new_gid != egid))
    {
        if (setregid (new_gid, new_gid))
        {
            perror ("Failed to switch group\n");
            return 1;
        }

        upd++;
    }

    if ((new_uid != uid) || (new_uid != euid))
    {
        if (setreuid (new_uid, new_uid))
        {
            perror ("Failed to switch user\n");
            return 1;
        }

        upd++;
    }

    if (0 == upd)
        return 0;

    if (false == bSetEnv)
        return 0;

    pPasswd = getpwuid (euid);

    if (NULL == pPasswd)
        return 1;

    if (pPasswd->pw_name)
    {
        setenv ("USER",   pPasswd->pw_name, 1);
        setenv ("LOGNAME", pPasswd->pw_name, 1);
    }

    if (pPasswd->pw_dir)
    {
        setenv ("HOME", pPasswd->pw_dir, 1);
    }

    return 0;
}

int SwitchToUser (bool bSetEnv)
{
    return SwitchUser (g_uid, g_gid, bSetEnv);
}


int SwitchToRealUser (bool bSetEnv)
{
    return SwitchUser (g_euid, g_egid, bSetEnv);
}


int SetEnvironmentVars()
{
    int  ret = 0;
    int  error = 0;
    bool bPassCmd = false;
    ssize_t ret_size     = 0;
    char szExe[2048]     = {0};
    char szCommand[2100] = {0};

    if (*g_szBorgRSH)
    {
        error = setenv ("BORG_RSH", g_szBorgRSH, 1);
        if (error)
            ret++;
    }

    if (*g_szBaseDir)
    {
        error = setenv ("BORG_BASE_DIR", g_szBaseDir, 1);
        if (error)
            ret++;
    }

    if (*g_szRemotePath)
    {
        error = setenv ("BORG_REMOTE_PATH", g_szRemotePath, 1);
        if (error)
            ret++;
    }

    if (*g_szPassCommand)
    {
        /* set own program as password command */
        if (0 == strcmp ("nshborg", g_szPassCommand))
        {
            ret_size = readlink ("/proc/self/exe", szExe, sizeof (szExe));

            if (ret_size)
            {
                snprintf (szCommand, sizeof (szCommand), "%s -GETPW", szExe);
            }
        }
        else
        {
            snprintf (szCommand, sizeof (szCommand), "%s", g_szPassCommand);
        }
    }

    if (*szCommand)
    {
        error = setenv ("BORG_PASSCOMMAND", szCommand, 1);
        if (error)
            ret++;
        else
            bPassCmd = true;
    }
    else
    {
        /* Only set pass phrase if no pass command is specified */
        if (*g_szPassphrase)
            error = setenv ("BORG_PASSPHRASE", g_szPassphrase, 1);
        else
            error = unsetenv ("BORG_PASSPHRASE");

        if (error)
            ret++;
    }

    if (false == bPassCmd)
    {
        error = unsetenv ("BORG_PASSCOMMAND");
    }

    if (*g_szBorgRepo)
        error = setenv ("BORG_REPO", g_szBorgRepo, 1);
    else
        error = unsetenv ("BORG_REPO");

    if (error)
        ret++;

    if (*g_szSSHAuthSock)
        error = setenv (g_szSSH_AUTH_SOCK, g_szSSHAuthSock, 1);
    else
        error = unsetenv (g_szSSH_AUTH_SOCK);

    if (error)
        ret++;

    return ret;
}


int UnsetEnvironmentVars()
{
    int ret = 0;
    int error = 0;

    error = unsetenv ("BORG_PASSPHRASE");

    if (error)
        ret++;

    error = unsetenv ("BORG_PASSCOMMAND");

    if (error)
        ret++;

    error = unsetenv ("BORG_REPO");

    if (error)
        ret++;

    return ret;
}


void SetNonBlockFD (int fd)
{
    int flags = 0;

    flags = fcntl( fd, F_GETFL, 0);
    fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}


pid_t popen3 (int *retpInputFD, int *retpOutputFD, int *retpErrorFD, int NonBlock, const char *argv[])
{
    int p_stdin[2]  = {0};
    int p_stdout[2] = {0};
    int p_stderr[2] = {0};
    pid_t pid = 0;

    if (pipe (p_stdin))
        return -1;

    if (pipe (p_stdout))
        return -1;

    if (pipe (p_stderr))
        return -1;

    if (NULL == argv[0])
        return -1;

    pid = fork();

    if (pid < 0)
    {
        /* error forking process */
        return pid;
    }
    else if (pid == 0)
    {
        /* child process */
        dup2 (p_stdin[READ], STDIN_FILENO);
        dup2 (p_stdout[WRITE], STDOUT_FILENO);
        dup2 (p_stderr[WRITE], STDERR_FILENO);

        /* close unused descriptors */
        close (p_stdin[READ]);
        close (p_stdout[READ]);
        close (p_stderr[READ]);

        close (p_stdin[WRITE]);
        close (p_stdout[WRITE]);
        close (p_stderr[WRITE]);

        /* switch child process to new binary */
        execv (argv[0], (char **) argv);

        /* this only reached when switching to the new binary did not work */
        perror ("Cannot run command");
        exit (1);
    }

    /* parent process */

    /* close unused descriptors on parent process*/
    close (p_stdin[READ]);
    close (p_stdout[WRITE]);
    close (p_stderr[WRITE]);

    /* close files or assign it to parent */

    if (retpInputFD)
    {
        *retpInputFD = p_stdin[WRITE];
    }
    else
    {
        close (p_stdin[WRITE]);
    }

    if (retpOutputFD)
    {
        *retpOutputFD = p_stdout[READ];
        if (NonBlock)
            SetNonBlockFD (*retpOutputFD);
    }
    else
    {
        close (p_stdout[READ]);
    }

    if (retpErrorFD)
    {
        *retpErrorFD = p_stderr[READ];

        if (NonBlock)
            SetNonBlockFD (*retpErrorFD);
    }
    else
    {
        close (p_stderr[READ]);
    }

    return pid;
}


int pclose3 (pid_t pid)
{
    int internal_stat = 0;

    waitpid (pid, &internal_stat, 0);
    return WEXITSTATUS (internal_stat);
}


time_t GetOSTimer()
{
  struct timeval t;

  gettimeofday (&t,NULL);
  return( (t.tv_sec*1000) + (t.tv_usec/1000) );
}


bool IsNullStr (const char *pszStr)
{
    if (NULL == pszStr)
        return 1;

    if ('\0' == *pszStr)
        return 1;

    return 0;
}


void strdncpy (char *pszStr, const char *ct, size_t n)
{
    if (NULL == pszStr)
        return;

    if (n>0)
    {
        strncpy (pszStr, ct, n-1);
        pszStr[n-1] = '\0';
    }
}


size_t GetFileSize (const char *pszFilename)
{
    int ret = 0;
    struct stat Filestat = {0};

    if (IsNullStr (pszFilename))
        return 0;

    ret = stat (pszFilename, &Filestat);

    if (ret)
        return 0;

    if (S_IFDIR & Filestat.st_mode)
        return 0;

    return Filestat.st_size;
}

size_t ReadFileIntoBuffer (const char *pszFilename, size_t BufferSize, char *retpszBuffer)
{
    size_t len       = 0;
    size_t bytesread = 0;
    
    FILE *fp = NULL;

    if (0 == BufferSize)
        return 0;

    if (retpszBuffer)
        *retpszBuffer = '\0';

    if (IsNullStr (pszFilename))
        return 0;

    len = GetFileSize (pszFilename);

    if (len >= BufferSize)
      len = BufferSize-1;

    fp = fopen (pszFilename, "r");

    if (NULL == fp)
    {
        printf ("Cannot read file: %s\n", pszFilename);
        goto Done;
    }

    bytesread = fread (retpszBuffer, 1, BufferSize, fp);
    retpszBuffer[bytesread] = '\0';

Done:

    if (fp)
    {
        fclose (fp);
        fp = NULL;
    }

    return len;
}

int FileExists (const char *pszFilename)
{
    int ret = 0;
    struct stat Filestat = {0};

    if (IsNullStr (pszFilename))
        return 0;

    ret = stat (pszFilename, &Filestat);

    if (ret)
        return 0;

    if (S_IFDIR & Filestat.st_mode)
        return 2;
    else
        return 1;
}


int CreateDirectoryTree (const char *pszFilename, mode_t SetMode)
{
    int    ret = 0;
    int    tmp = 0;
    int    len = 0;
    mode_t mode = 0;

    char   *pBuffer   = NULL;
    char   *p         = NULL;
    char   BackupChar = '\0';

    if (IsNullStr (pszFilename))
        return -1;

    if (FileExists (pszFilename))
        return 0;

    if (SetMode)
        mode = SetMode;
    else
        mode = S_IRWXU;

    /* Check if only this one directory is missing */
    if (0 == mkdir (pszFilename, mode))
    {
        goto Done;
    }

    /* Creating multiple directories */

    /* Create a work buffer in same size */
    pBuffer = strdup (pszFilename);
    len = strlen (pBuffer);

    if (len <= 1)
    {
        ret = 1;
        goto Done;
    }

    /* Ensure there is no trailing slash */
    p = pBuffer+len-1;
    p = pBuffer+len-1;
    if ('/' == *p)
        *p = '\0';

    p = pBuffer+1;

    while (*p)
    {
        while( *p && (*p != '/'))
            p++;

        /* Temporary remember char and replace it with '\0' */
        BackupChar = *p;
        *p = '\0';

        len = strlen (pBuffer);
        if (len < 1)
        {
            /* This is root directory */
            goto Done;
        }
        else
        {
            if (0 == FileExists (pBuffer))
            {
                ret = mkdir (pBuffer, mode);

                if (ret)
                {
                    printf ("Cannot create directory: [%s]\n", pBuffer);
                    goto Done;
                }
            }
            else
            {
                if (SetMode)
                {
                    tmp = chmod (pBuffer, SetMode);

                    if (tmp)
                    {
                        printf ("Info: Cannot change mode for [%s] to %o\n", pBuffer, mode);
                    }
                }
            }
        }

        if ('\0' == BackupChar)
            break;

        *p++ = BackupChar;
    } /* while */

Done:

    if (pBuffer)
    {
        free (pBuffer);
        pBuffer = NULL;
    }

    return ret;
}


int CreateFileDir (const char *pszFilename, mode_t SetMode)
{
    int  ret = 0;
    char *pBuffer = NULL;
    char *p       = NULL;
    char *pSlash  = NULL;

    if (IsNullStr (pszFilename))
    {
        ret = 1;
        goto Done;
    }

    pBuffer = strdup (pszFilename);

    p = pBuffer;
    pSlash = NULL;

    while (*p)
    {
        if ('/' == *p)
            pSlash = p;
        p++;
    }

    if (pSlash)
    {
        *pSlash = '\0';
        ret = CreateDirectoryTree (pBuffer, SetMode);
    }

Done:

    if (pBuffer)
    {
        free (pBuffer);
        pBuffer = NULL;
    }

    return ret;
}


pid_t CheckProcessRunning()
{
    int  count = 0;
    long pid   = 0;
    FILE *fp   = NULL;

    char szProcName[1024] = {0};

    if (g_Verbose)
        printf ("PID file: %s\n", g_szFilePID);

    fp = fopen (g_szFilePID, "r");

    if (NULL == fp)
    {
        /* No logging of no pid file */
        if (g_Verbose)
            perror ("Cannot open PID file");

        goto Done;
    }

    count = fscanf (fp, "%ld", &pid);

    if (1 != count)
    {
        printf ("Cannot read PID from: %s\n", g_szFilePID);
        goto Done;
    }

    snprintf (szProcName, sizeof (szProcName)-1, "/proc/%ld", pid);

    if (FileExists (szProcName) < 0)
    {
        printf ("nshborg process PID: %ld not found (%s)\n", pid, szProcName);
        pid = 0;
        goto Done;
    }

Done:

    if (fp)
    {
        fclose (fp);
        fp = NULL;
    }

    if (g_Verbose)
        printf ("nshborg process PID: %ld\n", pid);

    return pid;
}


int DumpLogFile (const char *pszFilename, bool bPrintHeader)
{
    int  ret = 0;
    FILE *fp = NULL;

    printf ("\n-----%s-----\n", pszFilename);

    fp = fopen (pszFilename, "r");

    if (NULL == fp)
    {
        printf ("Cannot open file [%s]\n", pszFilename);
        ret = 1;
        goto Done;
    }

    while ( fgets ((char *)g_Buffer, sizeof (g_Buffer)-1, fp) )
        printf ("%s", g_Buffer);

    if (bPrintHeader)
        printf ("-----%s-----\n\n", pszFilename);

Done:

    if (fp)
    {
        fclose (fp);
        fp = NULL;
    }

    return ret;
}


int WriteFilePID (const char *pszFilename)
{
    int ret = 0;
    FILE *fp    = NULL;

    if (IsNullStr (pszFilename))
    {
        ret = 1;
        goto Done;
    }

    fp = fopen (pszFilename, "w");

    if (NULL == fp)
    {
        ret = 1;
        perror ("Cannot create PID file");
        goto Done;
    }

    fprintf (fp, "%u", getpid());

Done:

    if (fp)
    {
        fclose (fp);
        fp = NULL;
    }

    return ret;
}


int BackupFile (int WriteFD, const char *pszFileName)
{
    int ret = 0;
    size_t  BytesRead    = 0;
    size_t  BytesWritten = 0;
    size_t  BytesTotal   = 0;
    size_t  BytesSize    = 0;

    pid_t  pid      =  0;
    int    InputFD  = -1;
    int    OutputFD = -1;
    int    ErrorFD  = -1;

    const char *args[] = { g_szTarBinary, "-cPf", "-", pszFileName, NULL };

    if (-1 == WriteFD)
    {
        printf ("Backup ERROR: No write file descriptor\n");
        ret = 1;
        goto Done;
    }

    if (IsNullStr (pszFileName))
    {
        printf ("Backup ERROR: No file specified\n");
        ret = 1;
        goto Done;
    }

    BytesSize = GetFileSize (pszFileName);

    if (0 == BytesSize)
    {
        printf ("Backup ERROR: Cannot backup empty files: %s\n", pszFileName);
        ret = 1;
        goto Done;
    }

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 0, args);

    if (pid < 1)
    {
        perror ("Backup ERROR: Cannot start tar process");
        ret = 1;
        goto Done;
    }

    while ((BytesRead = read (OutputFD, g_Buffer, sizeof (g_Buffer))))
    {
        BytesWritten = write (WriteFD, g_Buffer, BytesRead);

        if (BytesRead != BytesWritten)
        {
            printf ("Backup ERROR: Error writing buffer, Read: %lu, Written: %lu\n", BytesRead, BytesWritten);
            ret = 1;
            goto Done;
        }

        BytesTotal += BytesWritten;
    }

    BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
    if (BytesRead > 0)
    {
        g_Buffer[BytesRead] = '\0';
        printf ("\nBackup ERROR: Returned from tar\n\n");
        printf ("%s\n", g_Buffer);
    }

    printf ("Backup OK: [%s] %1.1f MB\n", pszFileName, BytesTotal/1024.0/1024.0);

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    return ret;
}


int BorgBackupPrune (long PruneDays)
{
    int   ret       = 0;
    pid_t pid       =  0;
    int   InputFD   = -1;
    int   OutputFD  = -1;
    int   ErrorFD   = -1;

    ssize_t BytesRead = 0;
    char  szPruneStr[255] = {0};

    const char *args[] = { g_szBorgBackupBinary, "prune", szPruneStr, NULL };

    if (PruneDays < g_MinPruneDays)
    {
        printf ("\nBackup ERROR: Prune not allowed for specified interval: %ld\n\n", PruneDays);
        return 1;
    }

    snprintf (szPruneStr, sizeof (szPruneStr), " --keep-within %ld%s", PruneDays, "d");

    SetEnvironmentVars();
    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 1, args);
    UnsetEnvironmentVars();

    if (pid < 1)
    {
        printf ("\nBackup ERROR: Cannot start Borg process\n\n");
        perror ("Backup ERROR: Cannot start Borg process");
        ret = 1;
        goto Done;
    }

    /* check if Borg process signaled an error */
    if (-1 != ErrorFD)
    {
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("\nBackup ERROR: Cannot start Borg process\n\n");
            printf ("%s\n", g_Buffer);
            goto Done;
        }
    }

    printf ("\nBackup OK: Prune successful\n\n");

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    return ret;
}


int BorgBackupDelete (const char *pszArchiv)
{
    int   ret       = 0;
    pid_t pid       =  0;
    int   InputFD   = -1;
    int   OutputFD  = -1;
    int   ErrorFD   = -1;

    ssize_t BytesRead = 0;

    const char *args[] = { g_szBorgBackupBinary, "delete", pszArchiv, NULL };

    if (0 == g_BorgDeleteAllowed)
    {
        printf ("\nBackup ERROR: Archive delete is not allowed\n\n");
        return 1;
    }

    if (IsNullStr (pszArchiv))
    {
        printf ("\nBackup ERROR: No archive to delete specified\n\n");
        ret = 1;
        goto Done;
    }

    SetEnvironmentVars();
    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 1, args);
    UnsetEnvironmentVars();

    if (pid < 1)
    {
        printf ("\nBackup ERROR: Cannot start Borg process\n\n");
        perror ("Backup ERROR: Cannot start Borg process");
        ret = 1;
        goto Done;
    }

    /* check if Borg process signaled an error */
    if (-1 != ErrorFD)
    {
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("\nBackup ERROR: Cannot start Borg process\n\n");
            printf ("%s\n", g_Buffer);
            goto Done;
        }
    }

    printf ("\nBackup OK: Prune successful\n\n");

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    return ret;
}


int BorgBackupStart (const char *pszReqFilename, const char *pszArchiv)
{
    int   ret       = 0;
    long  CountOK   = 0;
    long  CountErr  = 0;

    FILE  *fpReq    = NULL;
    FILE *fpLog     = NULL;

    pid_t CheckPID  = 0;
    pid_t pid       =  0;
    int   InputFD   = -1;
    int   OutputFD  = -1;
    int   ErrorFD   = -1;

    ssize_t BytesRead = 0;

    char   szFileName[MAX_PATH+1]    = {0};
    char   *p = NULL;

    const char *args[] = { g_szBorgBackupBinary, "import-tar", "--ignore-zeros", "--stats",  pszArchiv, "-", NULL };

    if (IsNullStr (pszReqFilename))
    {
        ret = 1;
        goto Done;
    }

    if (IsNullStr (pszArchiv))
    {
        ret = 1;
        goto Done;
    }

    CheckPID = CheckProcessRunning();

    if (CheckPID)
    {
        printf ("Backup ERROR: Backup process already running with PID %u\n", CheckPID);
        return 1;
    }

    printf ("Backup Archiv : %s\n", pszArchiv);
    printf ("Backup ReqFile: %s\n", pszReqFilename);

    /* Remove file if present */
    remove (pszReqFilename);

    printf ("\nStarting Borg process ...\n\n");

    SetEnvironmentVars();

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 1, args);

    UnsetEnvironmentVars();

    if (pid < 1)
    {
        printf ("\nBackup ERROR: Cannot start Borg process\n\n");
        perror ("Backup ERROR: Cannot start Borg process");
        ret = 1;
        goto Done;
    }

    printf ("Borg PID: %u\n", pid);

    sleep (2);

    /* check if Borg process signaled an error */
    if (-1 != ErrorFD)
    {
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("\nBackup ERROR: Cannot start Borg process\n\n");
            printf ("%s\n", g_Buffer);
            goto Done;
        }
    }

    printf ("Backup OK: BorgBackup started: %s\n", pszArchiv);

    /* Flush stdout before making the process a daemon */
    fflush(stdout);

    /* Switch process to a daemon process which isn't depending on calling process */
    CheckPID = daemon (1, 0);

    printf ("Daemon process has PID: %u\n", CheckPID);

    if (-1 == CheckPID)
    {
        ret = 1;
        printf ("\nBackup ERROR: Failed to turn process into a daemon!\n\n");
        perror ("Backup ERROR: Failed to turn process into a daemon!");
        goto Done;
    }

    WriteFilePID (g_szFilePID);

    while (1)
    {
        fpReq = fopen (pszReqFilename, "r");

        if (fpReq)
        {
            while ( fgets (szFileName, sizeof (szFileName)-1, fpReq) )
            {
                p = szFileName;
                while (*p)
                {
                    if ('\n' == *p)
                    {
                        *p = '\0';
                        break;
                    }
                    p++;
                }

                if ('\0' == *szFileName)
                    break;

                if (0 == strcmp (szFileName, g_szBackupEndMarker))
                {
                    printf ("Backup EndMarker found\n");
                    goto Done;
                }

                ret = BackupFile (InputFD, szFileName);
                if (ret)
                {
                    CountErr++;
                }
                else
                {
                    CountOK++;
                }
            }

            fclose (fpReq);
            fpReq = NULL;

            remove (pszReqFilename);
        }

        usleep (10*1000);

    } /* while */

Done:

    printf ("Done\n");

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (fpReq)
    {
        fclose (fpReq);
        fpReq = NULL;
    }

    sleep (1);

    fpLog = fopen (g_szBorgLogFile, "w");

    if (NULL == fpLog)
    {
        printf ("Backup ERROR: Cannot create file: %s\n", g_szBorgLogFile);
        goto Cleanup;
    }

    printf ("Created file: %s\n", g_szBorgLogFile);

    if (-1 != ErrorFD)
    {
        while (1)
        {
            BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
            if (BytesRead < 1)
                break;

            g_Buffer[BytesRead] = '\0';
            fprintf (fpLog, "%s", g_Buffer);
        }
    }

    if (-1 != OutputFD)
    {
        while (1)
        {
            BytesRead = read (OutputFD, g_Buffer, sizeof (g_Buffer)-1);
            if (BytesRead < 1)
                break;

            g_Buffer[BytesRead] = '\0';
            fprintf (fpLog, "%s", g_Buffer);
        }
    }

    fprintf (fpLog, "\n");
    if (CountErr)
        fprintf (fpLog, "Backup ERROR: BorgBackup completed with errors\n");
    else
        fprintf (fpLog, "Backup OK: BorgBackup completed\n");

    fprintf (fpLog, "-------------------------------\n");
    fprintf (fpLog, "Success: %4lu\n", CountOK);
    fprintf (fpLog, "Failure: %4lu\n", CountErr);
    fprintf (fpLog, "\n");

Cleanup:

    printf ("Cleanup\n");

    if (fpLog)
    {
        fclose (fpLog);
        fpLog = NULL;
    }

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    /* Finally remove request and PID file */
    remove (pszReqFilename);
    remove (g_szFilePID);

    return ret;
}


int WaitForFileDelete (const char *pszReqFile, long TimeoutSec)
{
    int   ret      = 0;
    long  WaitMsec = 0;
    struct stat sb = {0};

    if (IsNullStr (pszReqFile))
    {
        ret = 1;
        goto Done;
    }

    if (g_Verbose)
        printf ("Waiting %lu seconds for file delete: %s\n", TimeoutSec, pszReqFile);

    while (0 == ret)
    {
        ret = stat (pszReqFile, &sb);
        if (ret)
            break;

        usleep (g_WaitTime*1000);
        WaitMsec += g_WaitTime;

        if (TimeoutSec)
        {
            if (WaitMsec > (TimeoutSec*1000))
            {
                printf ("Timeout after %lu seconds\n", TimeoutSec);
                return 1;
            }
        }

    } /* while */

    if (g_Verbose)
        printf ("File deleted after %lu msec\n", WaitMsec);

Done:

    return 0;
}


int BackupFileToBorg (const char *pszFilename, const char *pszReqFile, long TimeoutSec)
{
    int ret = 0;
    FILE *fpReq    = NULL;
    struct stat sb = {0};
    bool bQuit     = false;
    time_t tStart  = {0};
    time_t tEnd    = {0};
    double sec     = 0.0;
    double mb      = 0.0;

    if (IsNullStr (pszFilename))
    {
        ret = 1;
        printf ("Backup ERROR: No file to backup specified\n");
        goto Done;
    }

    if (IsNullStr (pszReqFile))
    {
        ret = 1;
        printf ("Backup ERROR: No request file specified\n");
        goto Done;
    }

    if (0 == strcmp (pszFilename, g_szBackupEndMarker))
    {
        bQuit = true;
    }
    else
    {
        ret = stat (pszFilename, &sb);

        if (ret)
        {
            perror ("Cannot backup file");
            printf ("Backup ERROR: Cannot backup file: %s\n", pszFilename);
            // goto Done;
        }

        printf ("Backing up file %s, size: %ld bytes\n", pszFilename, sb.st_size);
    }

    if  (0 == CheckProcessRunning())
    {
        ret = 1;
        printf ("Backup ERROR: Backup process not running\n");
        goto Done;
    }

    fpReq = fopen (pszReqFile, "w");

    if (NULL == fpReq)
    {
        ret = 1;
        printf ("Backup ERROR: Cannot create request file: %s\n", pszReqFile);
    }

    fprintf (fpReq, "%s", pszFilename);

    fclose (fpReq);
    fpReq = NULL;

    tStart = GetOSTimer();

    ret = WaitForFileDelete (pszReqFile, TimeoutSec);

    if (ret)
    {
        printf ("Backup ERROR: %s\n", pszFilename);
        goto Done;
    }

    tEnd = GetOSTimer();

    if (bQuit)
    {
        DumpLogFile (g_szBorgLogFile, true);
        remove (g_szBorgLogFile);
        goto Done;
    }

    sec = (tEnd-tStart)/1000.0;
    mb  = sb.st_size/1024.0/1024.0;

    if (sec)
        printf ("Backup OK: %s, %1.1f MB (%1.1f MB/sec)\n", pszFilename, mb, mb/sec);
    else
        printf ("Backup OK: %s, %1.1f MB\n", pszFilename, mb);

Done:

    if (fpReq)
    {
        fclose (fpReq);
        fpReq = NULL;
    }

    return ret;
}


int BorgBackupInitRepo (const char *pszRepository)
{
    int ret = 0;
    int InputFD  = -1;
    int OutputFD = -1;
    int ErrorFD  = -1;

    pid_t   pid       =  0;
    ssize_t BytesRead = 0;

    const char *args[] = { g_szBorgBackupBinary, "init", "--encryption", "repokey", pszRepository, NULL };

    if (IsNullStr (pszRepository))
    {
        ret = 1;
        goto Done;
    }

    SetEnvironmentVars();

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 0, args);

    UnsetEnvironmentVars();

    if (pid < 1)
    {
        perror ("Restore ERROR: Cannot start Borg process");
        ret = 1;
        goto Done;
    }

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        /* Write potential error output into log */
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("%s\n", g_Buffer);
        }

        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    if (ret)
    {
        printf ("ERROR initializing archive\n");
    }

    return ret;
}


int GetTokenFromString (char *pszBuffer, const char *pszToken, int MaxValueSize, char *retpszValue)
{
    int  len = 0;
    char *p  = NULL;
    char *r  = retpszValue;

    if ((0 == MaxValueSize) || (NULL == pszBuffer) || (NULL == pszToken) || (NULL == retpszValue))
        return 0;

    p = strstr (pszBuffer, pszToken);

    if (NULL == p)
        return 0;

    MaxValueSize--;

    p += strlen (pszToken)+1;

    while (len < MaxValueSize)
    {
        if (';' == *p)
            break;

        *r = *p;
        r++;
        p++;
    }

    *r = '\0';

    return len;
}


int StartSSHAgent()
{
    int ret = 0;
    int InputFD  = -1;
    int OutputFD = -1;
    int ErrorFD  = -1;

    pid_t   pid  =  0;
    ssize_t BytesRead = 0;

    char szNum[20] = {0};
    const char *args[] = { g_szSSHAgentBinary, "-t", szNum, "-s", NULL };

    printf ("Starting SSH Agent ...\n");

    snprintf (szNum, sizeof (szNum), "%u", g_SSHKeyLife);

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 0, args);

    if (pid < 1)
    {
        perror ("Cannot start SSH Agent");
        ret = 1;
        goto Done;
    }

    printf ("SSH Agent started\n");

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        BytesRead = read (OutputFD, g_Buffer, sizeof (g_Buffer)-1);

        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';

            GetTokenFromString ((char *) g_Buffer, g_szSSH_AUTH_SOCK, sizeof (g_szSSHAuthSock), g_szSSHAuthSock);
            GetTokenFromString ((char *) g_Buffer, g_szSSH_AGENT_PID, sizeof (szNum), szNum);
            g_SSHAgentPID = atoi (szNum);

            if (g_Verbose)
                printf ("SSH Agent Sock: [%s] PID: %d\n", g_szSSHAuthSock, g_SSHAgentPID);
        }

        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        /* Write potential error output into log */
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("%s\n", g_Buffer);
        }

        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    if (ret)
    {
        printf ("ERROR starting SSH agent\n");
    }

    return ret;
}


int PushToSSHAgent()
{
    int ret = 0;
    int InputFD  = -1;
    int OutputFD = -1;
    int ErrorFD  = -1;

    pid_t   pid       = 0;
    ssize_t BytesRead = 0;

    const char *args[] = { g_szSSHAddBinary, "-", NULL };

    if (!*g_szSSHKey)
    {
        ret = 1;
        goto Done;
    }

    if (0 == g_SSHAgentPID)
    {
        ret = StartSSHAgent();
        if (ret)
            goto Done;
    }

    SetEnvironmentVars();

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 0, args);

    UnsetEnvironmentVars();

    if (pid < 1)
    {
        perror ("Cannot add SSH key");
        ret = 1;
        goto Done;
    }

    write (InputFD, g_szSSHKey, strlen (g_szSSHKey));

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        BytesRead = read (OutputFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("%s\n", g_Buffer);
        }

        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        /* Write potential error output into log */
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("%s\n", g_Buffer);
        }

        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    if (ret)
    {
        printf ("ERROR pushing key to SSH Agent\n");
    }

    return ret;
}


int BorgBackupRestore (const char *pszArchiv, const char *pszSource, const char *pszTarget)
{
    int ret = 0;
    pid_t pid      =  0;
    FILE *fpOutput = NULL;
    
    ssize_t BytesRead    = 0;
    ssize_t BytesWritten = 0;
    size_t  BytesTotal   = 0;

    time_t tStart   = {0};
    time_t tEnd     = {0};
    double sec      = 0.0;
    double mb       = 0.0;
    int    InputFD  = -1;
    int    OutputFD = -1;
    int    ErrorFD  = -1;

    const char *args[] = { g_szBorgBackupBinary, "extract", "--stdout", pszArchiv, pszSource , NULL };

    if (IsNullStr (pszArchiv))
    {
        ret = 1;
        goto Done;
    }

    if (IsNullStr (pszSource))
    {
        ret = 1;
        goto Done;
    }

    if (IsNullStr (pszTarget))
    {
        ret = 1;
        goto Done;
    }

    if (FileExists (pszTarget))
    {
        printf ("Restore ERROR: restoring from archive [%s] database [%s] to [%s] -- Target already exists\n", pszArchiv, pszSource, pszTarget);
        return 1;
    }

    ret = CreateFileDir (pszTarget, 0);

    fpOutput = fopen (pszTarget, "wb");

    if (NULL == fpOutput)
    {
        perror ("Restore ERROR: Cannot create target file");
        ret = 1;
        goto Done;
    }

    tStart = GetOSTimer();

    SetEnvironmentVars();

    pid = popen3 (&InputFD, &OutputFD, &ErrorFD, 0, args);

    UnsetEnvironmentVars();

    if (pid < 1)
    {
        perror ("Restore ERROR: Cannot start Borg process");
        ret = 1;
        goto Done;
    }

    while ((BytesRead = read (OutputFD, g_Buffer, sizeof (g_Buffer))))
    {
        BytesWritten = fwrite (g_Buffer, 1, BytesRead, fpOutput);

        if (BytesRead != BytesWritten)
        {
            printf ("Restore ERROR: Cannot write buffer, Read: %lu, Written: %lu\n", BytesRead, BytesWritten);
            ret = 1;
            goto Done;
        }

        BytesTotal += BytesWritten;
    }

    if (0 == BytesWritten)
    {
        ret = 1;
        goto Done;
    }

    tEnd = GetOSTimer();

    sec = (tEnd-tStart)/1000.0;
    mb  = BytesTotal/1024.0/1024.0;

    if (sec)
        printf ("Restore OK: %s -> %s, %1.1f MB (%1.1f MB/sec)\n", pszSource, pszTarget, mb, mb/sec);
    else
        printf ("Restore OK: %s -> %s, %1.1f MB\n", pszSource, pszTarget, mb);

Done:

    if (-1 != InputFD)
    {
        ret = close (InputFD);
        InputFD = -1;
    }

    if (-1 != OutputFD)
    {
        ret = close (OutputFD);
        OutputFD = -1;
    }

    if (-1 != ErrorFD)
    {
        /* Write potential error output into log */
        BytesRead = read (ErrorFD, g_Buffer, sizeof (g_Buffer)-1);
        if (BytesRead > 0)
        {
            g_Buffer[BytesRead] = '\0';
            printf ("%s\n", g_Buffer);
        }

        ret = close (ErrorFD);
        ErrorFD = -1;
    }

    if (pid > 0)
    {
        ret = pclose3 (pid);
        pid = 0;
    }

    if (fpOutput)
    {
        fclose (fpOutput);
        fpOutput = NULL;
    }

    if (ret)
    {
        printf ("ERROR restoring from archive [%s] database [%s] to [%s]\n", pszArchiv, pszSource, pszTarget);
        remove (pszTarget);
    }

    return ret;
}

int GetPassword()
{
    pid_t   ppid = getppid();
    ssize_t ret_size = 0;

    char szExe[2048]     = {0};
    char szProcess[2048] = {0};
 
    snprintf (szProcess, sizeof (szProcess), "/proc/%d/exe", ppid);

    ret_size = readlink (szProcess, szExe, sizeof (szExe));
    if (0 == ret_size)
        goto Done;

    if (strcmp (szExe, g_szBorgBackupBinary))
        goto Done;

    printf ("%s\n", g_szPassphrase);

Done:

    return 0;
}


int GetParam (const char *pszParamName, const char *pszName, const char *pszValue, int BufferSize, char *retpszBuffer)
{
    if (IsNullStr (pszName))
        return 0;

    if (NULL == pszValue)
        return 0;

    if (NULL == retpszBuffer)
        return 0;

    if (0 == BufferSize)
        return 0;

    if (strcmp (pszParamName, pszName))
        return 0;

    strdncpy (retpszBuffer, pszValue, BufferSize);
    return 1;
}


int ReadConfig (const char *pszConfigFile)
{
    int  ret = 0;
    FILE *fp = NULL;
    char *p  = NULL;
    char *pszValue = NULL;
    char szBuffer[4096] = {0};
    char szNum[20] = {0};

    if (IsNullStr (pszConfigFile))
    {
        fprintf (stderr, "No configuration file specified\n");
        ret = -1;
        goto Done;
    }

    if (0 == FileExists (pszConfigFile))
    {
        fprintf (stderr, "Info: No configuration profile found: %s\n", pszConfigFile);
    }

    fp = fopen (pszConfigFile, "r");

    if (NULL == fp)
    {
        fprintf (stderr, "Cannot open configuration file: %s\n", pszConfigFile);
        ret= -1;
        goto Done;
    }

    while ( fgets (szBuffer, sizeof (szBuffer)-1, fp) )
    {
        /* Parse for '=' to get value */
        p = szBuffer;
        pszValue = NULL;
        while (*p)
        {
            if ('=' == *p)
            {
                if (NULL == pszValue)
                {
                    *p = '\0';
                    pszValue = p+1;
                }
            }
            else if (*p < 32)
            {
               *p = '\0';
                break;
            }

            p++;
        }

        if (!*szBuffer)
            continue;

        if ('#' == *szBuffer)
            continue;

        if (NULL == pszValue)
        {
            fprintf (stdout, "Warning - Invalid parameter: [%s]\n", szBuffer);
            ret++;
            continue;
        }

             if ( GetParam ("directory",        szBuffer, pszValue, sizeof (g_nshBorgDir),         g_nshBorgDir));
        else if ( GetParam ("BORG_REPO",        szBuffer, pszValue, sizeof (g_szBorgRepo),         g_szBorgRepo));
        else if ( GetParam ("BORG_PASSPHRASE",  szBuffer, pszValue, sizeof (g_szPassphrase),       g_szPassphrase));
        else if ( GetParam ("BORG_PASSCOMMAND", szBuffer, pszValue, sizeof (g_szPassCommand),      g_szPassCommand));
        else if ( GetParam ("BORG_RSH",         szBuffer, pszValue, sizeof (g_szBorgRSH),          g_szBorgRSH));
        else if ( GetParam ("BORG_BASE_DIR",    szBuffer, pszValue, sizeof (g_szBaseDir),          g_szBaseDir));
        else if ( GetParam ("BORG_REMOTE_PATH", szBuffer, pszValue, sizeof (g_szRemotePath),       g_szRemotePath));
        else if ( GetParam ("BORG_BINARY",      szBuffer, pszValue, sizeof (g_szBorgBackupBinary), g_szBorgBackupBinary));
        else if ( GetParam ("SSH_KEYFILE",      szBuffer, pszValue, sizeof (g_szSSHKeyFile),       g_szSSHKeyFile));

        else if ( GetParam ("SSH_KEYLIFE", szBuffer, pszValue, sizeof (szNum), szNum))
        {
            g_SSHKeyLife = atoi (szNum);
        }
        else if ( GetParam ("BORG_DELETE_ALLOWED", szBuffer, pszValue, sizeof (szNum), szNum))
        {
            g_BorgDeleteAllowed = atoi (szNum);
        }
        else if ( GetParam ("BORG_MIN_PRUNE_DAYS", szBuffer, pszValue, sizeof (szNum), szNum))
        {
            g_MinPruneDays = atoi (szNum);
        }

        else
        {
             fprintf (stdout, "Warning - Invalid configuration parameter: [%s]\n", szBuffer);
             ret++;
        }

    } /* while */

Done:

    if (fp)
    {
        fclose (fp);
        fp = NULL;
    }

    return ret;
}


int main (int argc, char *argv[])
{
    int ret         = 0;
    int len         = 0;
    int  consumed   = 1;
    long TimeoutSec = 30*60;
    long PruneDays  = 0;
    bool bInitRepo  = false;

    char szDefaultReqFile[MAX_PATH+1] = {0};
    char szConfigFile[]       = "/etc/sysconfig/nshborg.cfg";
    char szDominoConfigFile[] = "/local/notesdata/domino/nshborg.cfg";

    const char *pszFilename = NULL;
    const char *pszArchiv   = NULL;
    const char *pszBackup   = NULL;
    const char *pszRestore  = NULL;
    const char *pszTarget   = NULL;
    const char *pszDelete   = NULL;
    const char *pszReqFile  = szDefaultReqFile;

    struct passwd *pPasswdEntry = NULL;

    if (0 == g_uid)
    {
        ret = 1;
        printf ("\nRunning as 'root' is not allowed!\n");
        goto Done;
    }

    /* Read configuration and SSH key with original user */

    ret = ReadConfig (szConfigFile);

    /* If standard config location not found, use the Domino specific one */
    if (ret < 0)
        ret = ReadConfig (szDominoConfigFile);

    if (ret < 0)
    {
        printf ("Info: No Borg configuration found. Using defaults\n");
    }

    pPasswdEntry = getpwuid (geteuid());

    if (0)
    {
        /* LATER: Check if we want to use a default Ed25519 key */
        if (!*g_szSSHKeyFile && pPasswdEntry)
        {
            snprintf (g_szSSHKeyFile, sizeof (g_szSSHKeyFile), "%s/.ssh/id_ed25519", pPasswdEntry->pw_dir);

            if (0 == FileExists (g_szSSHKeyFile))
                *g_szSSHKeyFile = '\0';
        }
    }

    /* Read SSH private key */
    if ((!*g_szSSHKey) && (*g_szSSHKeyFile))
    {
        printf ("Reading key: [%s]\n", g_szSSHKeyFile);

        if (*g_szSSHKeyFile)
        {
            len = ReadFileIntoBuffer (g_szSSHKeyFile, sizeof (g_szSSHKeyFile), g_szSSHKey);

            if (0 == len)
            {
                ret = 1;
                goto Done;
            }
        }
    }

    /* Switch to effective user */
    SwitchToUser (false);

    pPasswdEntry = getpwuid (geteuid());

    if (!*g_nshBorgDir)
        snprintf (g_nshBorgDir, sizeof (g_nshBorgDir), "%s/.nshborg", pPasswdEntry ? pPasswdEntry->pw_dir : "/tmp");

    snprintf (g_szFilePID,      sizeof (g_szFilePID),      "%s/nshborg.pid",  g_nshBorgDir);
    snprintf (g_szBorgLogFile,  sizeof (g_szBorgLogFile),  "%s/nshborg.log",  g_nshBorgDir);
    snprintf (szDefaultReqFile, sizeof (szDefaultReqFile), "%s/.nshborg.reg", g_nshBorgDir);

    CreateDirectoryTree (g_nshBorgDir, S_IRWXU);

    while (argc > consumed)
    {
        if (0 == strcmp (argv[consumed], "--version"))
        {
            printf ("%s\n", g_szVersion);
            goto Done;
        }

        else if (0 == strcmp (argv[consumed], "-i"))
        {
            bInitRepo = true;
        }

        else if (0 == strcmp (argv[consumed], "-z"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszReqFile = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-a"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszArchiv = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-b"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszBackup = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-r"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszRestore = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-t"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszTarget = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-o"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            snprintf (g_szBorgRepo, sizeof (g_szBorgRepo)-1, "%s", argv[consumed]);
        }

        else if (0 == strcmp (argv[consumed], "-w"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            TimeoutSec = atoi (argv[consumed]);
        }

        else if (0 == strcmp (argv[consumed], "-q"))
        {
            pszFilename = g_szBackupEndMarker;
        }

        else if (0 == strcmp (argv[consumed], "-v"))
        {
            g_Verbose++;
        }

        else if (0 == strcmp (argv[consumed], "-GETPW"))
        {
            GetPassword();
            goto Done;
        }

        else if (0 == strcmp (argv[consumed], "-delete"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            pszDelete = argv[consumed];
        }

        else if (0 == strcmp (argv[consumed], "-prune"))
        {
            consumed++;
            if (consumed >= argc)
                goto InvalidSyntax;
            if (argv[consumed][0] == '-')
                goto InvalidSyntax;

            PruneDays = atoi (argv[consumed]);
        }

        else
        {
            if ('-' == *argv[consumed])
                goto InvalidSyntax;

            pszFilename = argv[consumed];
        }

        consumed++;
    } /* while */

    if (pszFilename)
    {
        ret = BackupFileToBorg (pszFilename, pszReqFile, TimeoutSec);
        goto Done;
    }

    ret = PushToSSHAgent();

    if (bInitRepo)
    {
        ret = BorgBackupInitRepo (g_szBorgRepo);
        goto Done;
    }

    if (pszRestore)
    {
        ret = BorgBackupRestore (pszArchiv, pszRestore, pszTarget);
        goto Done;
    }

    if (pszBackup)
    {
        ret = BorgBackupStart (pszReqFile, pszBackup);
        goto Done;
    }

    if (PruneDays)
    {
        ret = BorgBackupPrune (PruneDays);
        goto Done;
    }

    if (pszDelete)
    {
        ret = BorgBackupDelete (pszDelete);
        goto Done;
    }

    goto Done;


InvalidSyntax:

    printf ("\nInvalid Syntax!\n\n");

Done:


    if (g_SSHAgentPID)
    {
        kill (g_SSHAgentPID, SIGKILL);
        g_SSHAgentPID = 0;
    }

    return ret;
}
