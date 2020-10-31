/************************************************************************/
/* globus_gridftp_server_posix.c                                        */
/*                                                                      */
/* Auther: Wei Yang (Stanford Linear Accelerator Center, 2007)          */
/*                                                                      */
/* Globus Gridftp 4.x Data Storage Interface module using POSIX IO      */
/*                                                                      */
/* The following functions are copied from the original globus DSI      */
/* module for "file"                                                    */
/*                                                                      */
/*     globus_l_gfs_file_copy_stat()                                    */
/*     globus_l_gfs_file_destroy_stat()                                 */
/*     globus_l_gfs_file_partition_path()                               */
/*     globus_l_gfs_posix_stat()                                        */
/*     globus_l_gfs_file_delete_dir()                                   */
/*                                                                      */
/************************************************************************/

/* ChangeLog:

   2009-03-16: Wei Yang  yangw@slac.stanford.edu
      *  add Adler32 checksum. (need -lz when linking the .so)
   2016-02-23: Wei Yang  yangw@slac.stanford.edu
      *  Return errno for various operations
   2017-04-13: Wei Yang  yangw@slac.stanford.edu
      *  add MD5 checksum (need -lssl when linking the .so)
      *  Add GRIDFTP_APPEND_XROOTD_CGI
   2017-05-02: Wei Yang  yangw@slac.stanford.edu
      *  add support to GLOBUS_GFS_CMD_TRNC 
                        GLOBUS_GFS_CMD_SITE_RDEL
                        GLOBUS_GFS_CMD_SITE_CHGRP
                        GLOBUS_GFS_CMD_SITE_UTIME
                        GLOBUS_GFS_CMD_SITE_SYMLINK
   2020-09-28: Wei Yang  yangw@slac.stanford.edu
      *  send progress mark during internal MD5 checksuming

 */

/* $Id: $ */

/************************************************************************/
/* How to compile:                                                      */
/*                                                                      */
/* This file should be compiled along with the globus 4.0.x source code.*/
/* Please copy this file to source-trees/gridftp/server/src/dsi_bones   */
/* and make adjustment to the Makefile. You may want to read the        */
/* README.txt file in that directory first. It will be compiled to a    */
/* shared library file libglobus_gridftp_server_posix_gcc32dbg.so.      */
/************************************************************************/
/* How to use it with xrootd:                                           */
/*                                                                      */
/* This code does not use any Xrootd specific functions. So it should   */
/* work with other types of storage in principle. However, we only      */
/* tested it with xrootd. We have used it under vdt1.3.9, 1.6.1 and     */
/* OSG 0.4.0, 0.6.                                                      */
/*                                                                      */
/* The idea is to overload the Posix IO functions by those provided in  */
/* the xrootd Posix interface. To do it, modify VDT's gridftp start up  */ 
/* script to something like this:                                       */
/*                                                                      */
/* #!/bin/sh                                                            */
/*                                                                      */
/* . /opt/vdt/setup.sh                                                  */
/*                                                                      */
/* XRDLIB="/path_to_xrootd_lib_dir"                                     */
/* if [ -z "$LD_LIBRARY_PATH" ]; then                                   */
/*     export LD_LIBRARY_PATH="$XRDLIB"                                 */
/* else                                                                 */
/*     export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:$XRDLIB"              */
/* fi                                                                   */
/* export LD_PRELOAD=$XRDLIB/libXrdPosixPreload.so                      */
/* export XROOTD_VMP="host:1094:/path1=/path2"                          */
/*                                                                      */
/* exec /opt/vdt/globus/sbin/globus-gridftp-server -dsi posix           */
/*                                                                      */
/* Note that the option in the last line (-dsi posix) will load         */
/* libglobus_gridftp_server_posix_gcc32dbg.so, So the .so should be in  */
/* the search path of LD_LIBRARY_PATH                                   */
/*                                                                      */
/* Please refer to src/XrdPosix/README for description on how to use    */
/* environment variable XROOTD_VMP                                      */
/************************************************************************/

/* 
   ATLAS FTS adds an extra '/' before path. This cause a problem to xrootd
   based storage because it uses a environment varialbe XROOTD_VMP to translate 
   gsiftp url to xrootd path. So these leading '/'s should be removed. Here
   is the psuedo-code:

   while (pathname[0] == '/' && pathname[1] == '/') { pathname++; }
*/

#include <unistd.h>
#include <sys/types.h>
#include <grp.h>
#include <utime.h>
#include <dirent.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/md5.h>
#include "globus_gridftp_server.h"

static
globus_version_t local_version =
{
    0, /* major version number */
    1, /* minor version number */
    1170189432,
    0 /* branch ID */
};

typedef struct globus_l_gfs_posix_handle_s
{
    char *                              pathname; 
    int                                 fd;
    char                                seekable;
    globus_size_t                       block_size;
    globus_off_t                        block_length;
    globus_off_t                        offset;
    globus_bool_t                       done;
    globus_gfs_operation_t              op;
    int                                 optimal_count;
    int                                 outstanding;
    globus_mutex_t                      mutex;
} globus_l_gfs_posix_handle_t;

char err_msg[256];
static int local_io_block_size = 0;
static int local_io_count = 0;

/*************************************************************************
 *  start
 *  -----
 *  This function is called when a new session is initialized, ie a user 
 *  connectes to the server.  This hook gives the dsi an oppertunity to
 *  set internal state that will be threaded through to all other
 *  function calls associated with this session.  And an oppertunity to
 *  reject the user.
 *
 *  finished_info.info.session.session_arg should be set to an DSI
 *  defined data structure.  This pointer will be passed as the void *
 *  user_arg parameter to all other interface functions.
 * 
 *  NOTE: at nice wrapper function should exist that hides the details 
 *        of the finished_info structure, but it currently does not.  
 *        The DSI developer should jsut follow this template for now
 ************************************************************************/
static
void
globus_l_gfs_posix_start(
    globus_gfs_operation_t              op,
    globus_gfs_session_info_t *         session_info)
{
    globus_l_gfs_posix_handle_t *       posix_handle;
    globus_gfs_finished_info_t          finished_info;
    struct passwd *                     pw;

    GlobusGFSName(globus_l_gfs_posix_start);

    globus_gridftp_server_set_checksum_support(op, "MD5:10");

    posix_handle = (globus_l_gfs_posix_handle_t *)
        globus_malloc(sizeof(globus_l_gfs_posix_handle_t));

    posix_handle->fd = 0;

    memset(&finished_info, '\0', sizeof(globus_gfs_finished_info_t));
    finished_info.type = GLOBUS_GFS_OP_SESSION_START;
    finished_info.result = GLOBUS_SUCCESS;
    finished_info.info.session.session_arg = posix_handle;
    finished_info.info.session.username = session_info->username;
    pw = getpwuid(getuid());
    finished_info.info.session.home_dir = pw->pw_dir;

    globus_gridftp_server_operation_finished(
        op, GLOBUS_SUCCESS, &finished_info);
}

/*************************************************************************
 *  destroy
 *  -------
 *  This is called when a session ends, ie client quits or disconnects.
 *  The dsi should clean up all memory they associated wit the session
 *  here. 
 ************************************************************************/
static
void
globus_l_gfs_posix_destroy(
    void *                              user_arg)
{
    globus_l_gfs_posix_handle_t *       posix_handle;

    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    globus_free(posix_handle);
}

/*************************************************************************
 *  stat
 *  ----
 *  This interface function is called whenever the server needs 
 *  information about a given file or resource.  It is called then an
 *  LIST is sent by the client, when the server needs to verify that 
 *  a file exists and has the proper permissions, etc.
 ************************************************************************/
/*
static
void
globus_l_gfs_posix_stat(
    globus_gfs_operation_t              op,
    globus_gfs_stat_info_t *            stat_info,
    void *                              user_arg)
{
    globus_gfs_stat_t                   stat_array[1];
    int                                 stat_count = 1;
    globus_l_gfs_posix_handle_t *      posix_handle;
    struct stat                         statbuf; 
    globus_result_t                     rc;

    GlobusGFSName(globus_l_gfs_posix_stat);

    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;
    if (stat(PathName, &statbuf) == 0)    
    {
        stat_array[0].mode = statbuf.st_mode;
        stat_array[0].nlink = statbuf.st_nlink;
        stat_array[0].uid = statbuf.st_uid;
        stat_array[0].gid = statbuf.st_gid;
        stat_array[0].size = statbuf.st_size;
        stat_array[0].mtime = statbuf.st_mtime;
        stat_array[0].atime = statbuf.st_atime;
        stat_array[0].ctime = statbuf.st_ctime;
        stat_array[0].dev = statbuf.st_dev;
        stat_array[0].ino = statbuf.st_ino;

        globus_gridftp_server_finished_stat(
            op, GLOBUS_SUCCESS, stat_array, stat_count);
    }
    else
    {
        rc = GlobusGFSErrorGeneric("stat() fail");
        globus_gridftp_server_finished_stat(op, rc, NULL, 0);
    }
}
*/

void
globus_l_gfs_file_copy_stat(
    globus_gfs_stat_t *                 stat_object,
    struct stat *                       stat_buf,
    const char *                        filename,
    const char *                        symlink_target)
{
    GlobusGFSName(globus_l_gfs_file_copy_stat);

    stat_object->mode     = stat_buf->st_mode;
    stat_object->nlink    = stat_buf->st_nlink;
    stat_object->uid      = stat_buf->st_uid;
    stat_object->gid      = stat_buf->st_gid;
    stat_object->size     = stat_buf->st_size;
    stat_object->mtime    = stat_buf->st_mtime;
    stat_object->atime    = stat_buf->st_atime;
    stat_object->ctime    = stat_buf->st_ctime;
    stat_object->dev      = stat_buf->st_dev;
    stat_object->ino      = stat_buf->st_ino;

    if(filename && *filename)
    {
        stat_object->name = strdup(filename);
    }
    else
    {
        stat_object->name = NULL;
    }
    if(symlink_target && *symlink_target)
    {
        stat_object->symlink_target = strdup(symlink_target);
    }
    else
    {
        stat_object->symlink_target = NULL;
    }
}

static
void
globus_l_gfs_file_destroy_stat(
    globus_gfs_stat_t *                 stat_array,
    int                                 stat_count)
{
    int                                 i;
    GlobusGFSName(globus_l_gfs_file_destroy_stat);

    for(i = 0; i < stat_count; i++)
    {
        if(stat_array[i].name != NULL)
        {
            globus_free(stat_array[i].name);
        }
        if(stat_array[i].symlink_target != NULL)
        {
            globus_free(stat_array[i].symlink_target);
        }
    }
    globus_free(stat_array);
}

/* basepath and filename must be MAXPATHLEN long
 * the pathname may be absolute or relative, basepath will be the same */
static
void
globus_l_gfs_file_partition_path(
    const char *                        pathname,
    char *                              basepath,
    char *                              filename)
{
    char                                buf[MAXPATHLEN];
    char *                              filepart;
    GlobusGFSName(globus_l_gfs_file_partition_path);

    strncpy(buf, pathname, MAXPATHLEN);
    buf[MAXPATHLEN - 1] = '\0';

    filepart = strrchr(buf, '/');
    while(filepart && !*(filepart + 1) && filepart != buf)
    {
        *filepart = '\0';
        filepart = strrchr(buf, '/');
    }

    if(!filepart)
    {
        strcpy(filename, buf);
        basepath[0] = '\0';
    }
    else
    {
        if(filepart == buf)
        {
            if(!*(filepart + 1))
            {
                basepath[0] = '\0';
                filename[0] = '/';
                filename[1] = '\0';
            }
            else
            {
                *filepart++ = '\0';
                basepath[0] = '/';
                basepath[1] = '\0';
                strcpy(filename, filepart);
            }
        }
        else
        {
            *filepart++ = '\0';
            strcpy(basepath, buf);
            strcpy(filename, filepart);
        }
    }
}


static
void
globus_l_gfs_posix_stat(
    globus_gfs_operation_t              op,
    globus_gfs_stat_info_t *            stat_info,
    void *                              user_arg)
{
    globus_result_t                     result;
    struct stat                         stat_buf;
    globus_gfs_stat_t *                 stat_array;
    int                                 stat_count = 0;
    DIR *                               dir;
    char                                basepath[MAXPATHLEN];
    char                                filename[MAXPATHLEN];
    char                                symlink_target[MAXPATHLEN];
    char *                              PathName;
    GlobusGFSName(globus_l_gfs_posix_stat);
    PathName=stat_info->pathname;

   /* 
      If we do stat_info->pathname++, it will cause third-party transfer
      hanging if there is a leading // in path. Don't know why. To work
      around, we replaced it with PathName.
   */
    while (PathName[0] == '/' && PathName[1] == '/')
    {
        PathName++;
    }
    
    /* lstat is the same as stat when not operating on a link */
    if(lstat(PathName, &stat_buf) != 0)
    {
        result = GlobusGFSErrorSystemError("stat", errno);
        goto error_stat1;
    }
    /* if this is a link we still need to stat to get the info we are 
        interested in and then use realpath() to get the full path of 
        the symlink target */
    *symlink_target = '\0';
    if(S_ISLNK(stat_buf.st_mode))
    {
        if(stat(PathName, &stat_buf) != 0)
        {
            result = GlobusGFSErrorSystemError("stat", errno);
            goto error_stat1;
        }
        if(realpath(PathName, symlink_target) == NULL)
        {
            result = GlobusGFSErrorSystemError("realpath", errno);
            goto error_stat1;
        }
    }    
    globus_l_gfs_file_partition_path(PathName, basepath, filename);
    
    if(!S_ISDIR(stat_buf.st_mode) || stat_info->file_only)
    {
        stat_array = (globus_gfs_stat_t *)
            globus_malloc(sizeof(globus_gfs_stat_t));
        if(!stat_array)
        {
            result = GlobusGFSErrorMemory("stat_array");
            goto error_alloc1;
        }
        
        globus_l_gfs_file_copy_stat(
            stat_array, &stat_buf, filename, symlink_target);
        stat_count = 1;
    }
    else
    {
        struct dirent *                 dir_entry;
        int                             i;
        char                            dir_path[MAXPATHLEN];
    
        dir = opendir(PathName);
        if(!dir)
        {
            result = GlobusGFSErrorSystemError("opendir", errno);
            goto error_open;
        }
        
        stat_count = 0;
        while(globus_libc_readdir_r(dir, &dir_entry) == 0 && dir_entry)  
        {
            stat_count++;
            globus_free(dir_entry);
        }
        
        rewinddir(dir);
        
        stat_array = (globus_gfs_stat_t *)
            globus_malloc(sizeof(globus_gfs_stat_t) * stat_count);
        if(!stat_array)
        {
            result = GlobusGFSErrorMemory("stat_array");
            goto error_alloc2;
        }

        snprintf(dir_path, sizeof(dir_path), "%s/%s", basepath, filename);
        dir_path[MAXPATHLEN - 1] = '\0';
        
        for(i = 0;
            globus_libc_readdir_r(dir, &dir_entry) == 0 && dir_entry;  
            i++)
        {
            char                        tmp_path[MAXPATHLEN];
            char                        *path;
                
            snprintf(tmp_path, sizeof(tmp_path), "%s/%s", dir_path, dir_entry->d_name);
            tmp_path[MAXPATHLEN - 1] = '\0';
            path=tmp_path;
        
            /* function globus_l_gfs_file_partition_path() seems to add two 
               extra '/'s to the beginning of tmp_path. XROOTD is sensitive 
               to the extra '/'s not defined in XROOTD_VMP so we remove them */
            if (path[0] == '/' && path[1] == '/') { path++; }
            while (path[0] == '/' && path[1] == '/') { path++; }
            /* lstat is the same as stat when not operating on a link */
            if(lstat(path, &stat_buf) != 0)
            {
                result = GlobusGFSErrorSystemError("lstat", errno);
                globus_free(dir_entry);
                /* just skip invalid entries */
                stat_count--;
                i--;
                continue;
            }
            /* if this is a link we still need to stat to get the info we are 
                interested in and then use realpath() to get the full path of 
                the symlink target */
            *symlink_target = '\0';
            if(S_ISLNK(stat_buf.st_mode))
            {
                if(stat(path, &stat_buf) != 0)
                {
                    result = GlobusGFSErrorSystemError("stat", errno);
                    globus_free(dir_entry);
                    /* just skip invalid entries */
                    stat_count--;
                    i--;
                    continue;
                }
                if(realpath(path, symlink_target) == NULL)
                {
                    result = GlobusGFSErrorSystemError("realpath", errno);
                    globus_free(dir_entry);
                    /* just skip invalid entries */
                    stat_count--;
                    i--;
                    continue;
                }
            }    
            globus_l_gfs_file_copy_stat(
                &stat_array[i], &stat_buf, dir_entry->d_name, symlink_target);
            globus_free(dir_entry);
        }
        
        if(i != stat_count)
        {
            result = GlobusGFSErrorSystemError("readdir", errno);
            goto error_read;
        }
        
        closedir(dir);
    }
    
    globus_gridftp_server_finished_stat(
        op, GLOBUS_SUCCESS, stat_array, stat_count);
    
    
    globus_l_gfs_file_destroy_stat(stat_array, stat_count);
    
    return;

error_read:
    globus_l_gfs_file_destroy_stat(stat_array, stat_count);
    
error_alloc2:
    closedir(dir);
    
error_open:
error_alloc1:
error_stat1:
    globus_gridftp_server_finished_stat(op, result, NULL, 0);

/*    GlobusGFSFileDebugExitWithError();  */
}

/* recursively delete a directory */
static
globus_result_t
globus_l_gfs_file_delete_dir(
    const char *                        pathname)
{

    globus_result_t                     result;
    int                                 rc;
    DIR *                               dir;
    struct stat                         stat_buf;
    struct dirent *                     dir_entry;
    int                                 i;
    char                                path[MAXPATHLEN];
    GlobusGFSName(globus_l_gfs_file_delete_dir);
    GlobusGFSFileDebugEnter();
    
    /* lstat is the same as stat when not operating on a link */
    if(lstat(pathname, &stat_buf) != 0)
    {
        result = GlobusGFSErrorSystemError("stat", errno);
        goto error_stat;
    }
    
    if(!S_ISDIR(stat_buf.st_mode))
    {
        /* remove anything that isn't a dir -- don't follow links */
        rc = unlink(pathname);       
        if(rc != 0)
        {
            result = GlobusGFSErrorSystemError("unlink", errno);
            goto error_unlink1;
        }
    }
    else
    {
        dir = globus_libc_opendir(pathname);
        if(!dir)
        {
            result = GlobusGFSErrorSystemError("opendir", errno);
            goto error_open;
        }
        
        for(i = 0;
            globus_libc_readdir_r(dir, &dir_entry) == 0 && dir_entry;
            i++)
        {   
            if(dir_entry->d_name[0] == '.' && 
                (dir_entry->d_name[1] == '\0' || 
                (dir_entry->d_name[1] == '.' && dir_entry->d_name[2] == '\0')))
            {
                globus_free(dir_entry);
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s", pathname, dir_entry->d_name);
            path[MAXPATHLEN - 1] = '\0';
              
            /* lstat is the same as stat when not operating on a link */
            if(lstat(path, &stat_buf) != 0)
            {
                result = GlobusGFSErrorSystemError("lstat", errno);
                globus_free(dir_entry);
                /* just skip invalid entries */
                continue;
            }
            
            if(!S_ISDIR(stat_buf.st_mode))
            {
                /* remove anything that isn't a dir -- don't follow links */
                rc = unlink(path);       
                if(rc != 0)
                {
                    result = GlobusGFSErrorSystemError("unlink", errno);
                    goto error_unlink2;
                }
            }
            else
            {
                result = globus_l_gfs_file_delete_dir(path);
                if(result != GLOBUS_SUCCESS)
                {
                    goto error_recurse;
                }
            }

            globus_free(dir_entry);
        }

        closedir(dir);
        rc = rmdir(pathname);
        if(rc != 0)
        {
            result = GlobusGFSErrorSystemError("rmdir", errno);
            goto error_rmdir;
        }
    } 
    
    GlobusGFSFileDebugExit();
    return GLOBUS_SUCCESS;

error_recurse:
error_unlink2:
        closedir(dir);
        globus_free(dir_entry);
error_open: 
error_stat:
error_unlink1:
error_rmdir:    
    GlobusGFSFileDebugExitWithError();
    return result; 
}

/* change group */
static
globus_result_t
globus_l_gfs_posix_chgrp(
    const char *                        pathname,
    const char *                        group)
{
    int                                 rc;
    globus_result_t                     result;
    struct group *                      grp_info;
    int                                 grp_id;
    char*                               endpt;

    GlobusGFSName(globus_l_gfs_posix_chgrp);

    grp_info = getgrnam(group);
    if(grp_info != NULL)
    {
        grp_id = grp_info->gr_gid;
    }
    else 
    {
        grp_id = strtol(group, &endpt, 10);
        if(*group == '\0' || *endpt != '\0') 
        {
            errno = EPERM;
            return GLOBUS_FAILURE; 
        }
    }
    if(grp_id < 0) {
	errno = EPERM;
        return GLOBUS_FAILURE;
    }
    if (chown(pathname, -1, grp_id) != 0) return GLOBUS_FAILURE;
    return GLOBUS_SUCCESS;
}

/* cksm from external call main initially include path */
char    cksm[512];

/*************************************************************************
 * Adler23 checksum
 ************************************************************************/
globus_result_t 
globus_l_gfs_posix_cksm_adler32(
    globus_gfs_operation_t             op,
    char *                             filename)
{
    int rc, fd, len;
    char *ext_adler32, buf[65536], ext_cmd[1024], *pt;
    FILE *F;
    struct stat stbuf;
    uLong adler;

    ext_adler32 = NULL;
    if ((ext_adler32 = getenv("GRIDFTP_CKSUM_EXT_ADLER32")) != NULL)
    {
        strcpy(ext_cmd, ext_adler32);
        strcat(ext_cmd, " ");
        strcat(ext_cmd, filename);
        F = popen(ext_cmd, "r");
        if (F == NULL) return GLOBUS_FAILURE;
        fscanf(F, "%s", cksm);
        pclose(F);

        pt = strchr(cksm, ' ');
        if (pt != NULL) pt[0] = '\0'; /* take the first string */ 
    }
    else /* calculate adler32 */
    {
        rc = stat(filename, &stbuf);
        if (rc != 0 || ! S_ISREG(stbuf.st_mode) || (fd = open(filename,O_RDONLY)) < 0)
            return GLOBUS_FAILURE;
        adler = adler32(0L, Z_NULL, 0);
        while ((len = read(fd, buf, 65536)) > 0)
            adler = adler32(adler, buf, len);

        close(fd);
        sprintf(cksm, "%08x", adler);
        cksm[8] = '\0';
    }

    globus_gridftp_server_finished_command(op, GLOBUS_SUCCESS, cksm);       
    return GLOBUS_SUCCESS;
}

/*************************************************************************
 * MD5 checksum
 ************************************************************************/

struct globus_l_gfs_posix_cksm_md5_cb_t
{
    globus_gfs_operation_t             op;
    MD5_CTX                            c;
    int                                fd;
    globus_off_t                       fsize;
    globus_off_t                       blocksize;
    globus_off_t                       offset;
    globus_off_t                       length;
    globus_off_t                       total_bytes;
    int                                marker_freq;
    time_t                             t_lastmarker;
};

#define MAXBLOCSIZE4CKSM 4*1024*1024

void 
globus_l_gfs_posix_cksm_md5_cb(
    void *                             user_arg)
{
    char                               buffer[MAXBLOCSIZE4CKSM+1];
    globus_off_t                       readlen;
    globus_result_t                    result;
    unsigned char                      md5digest[MD5_DIGEST_LENGTH];
    int                                i;
    time_t                             t;

    struct globus_l_gfs_posix_cksm_md5_cb_t * md5updt;

    md5updt = (struct globus_l_gfs_posix_cksm_md5_cb_t *) user_arg;
    if ( md5updt->length == 0 )
    {
        close(md5updt->fd);

        MD5_Final(md5digest, &(md5updt->c));
        for(i = 0; i < MD5_DIGEST_LENGTH; ++i)
            sprintf(&cksm[i*2], "%02x", (unsigned int)md5digest[i]);
        cksm[MD5_DIGEST_LENGTH*2+1] = '\0';

        globus_gridftp_server_finished_command(md5updt->op, GLOBUS_SUCCESS, cksm);       
        globus_free(md5updt);
    }
    else
    {
        readlen = read(md5updt->fd, buffer, ( md5updt->length > md5updt->blocksize ?
                                          md5updt->blocksize : md5updt->length ) );
        if (readlen > 0 && errno == 0)
        {
            md5updt->offset += readlen;
            md5updt->length -= readlen;
            md5updt->total_bytes += readlen;

            MD5_Update(&(md5updt->c), buffer, readlen);

            t = time(NULL);
            if ( (t - md5updt->t_lastmarker) > md5updt->marker_freq )
            {
                md5updt->t_lastmarker = t;
                char                        count[128];
                sprintf(count, "%"GLOBUS_OFF_T_FORMAT, md5updt->total_bytes);
                globus_gridftp_server_intermediate_command(md5updt->op, GLOBUS_SUCCESS, count);
            }
        }
        else
        {   // something isn't right, we will end getting a timeout
            errno = 0;
            sleep(2);
        }

        result = globus_callback_register_oneshot( NULL,
                                                   NULL,
                                                   globus_l_gfs_posix_cksm_md5_cb,
                                                   md5updt);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_callback_register_oneshot", result);
            globus_panic(NULL, result, "oneshot failed, no recovery");
        }
    }
}

globus_result_t 
globus_l_gfs_posix_cksm_md5(
    globus_gfs_operation_t             op,
    char *                             filename,
    globus_off_t                       offset,
    globus_off_t                       length)
{
    char *ext_md5, ext_cmd[1024], *pt;
    FILE *F;

    int rc, fd;
    struct stat stbuf;
    globus_result_t                    result;

    ext_md5 = NULL;
    if ((ext_md5 = getenv("GRIDFTP_CKSUM_EXT_MD5")) != NULL)
    {
        strcpy(ext_cmd, ext_md5);
        strcat(ext_cmd, " ");
        strcat(ext_cmd, filename);
        F = popen(ext_cmd, "r");
        if (F == NULL) return GLOBUS_FAILURE;
        fscanf(F, "%s", cksm);
        pclose(F);

        pt = strchr(cksm, ' ');
        if (pt != NULL) pt[0] = '\0'; /* take the first string */ 

        globus_gridftp_server_finished_command(op, GLOBUS_SUCCESS, cksm);       
    }
    else /* calculate md5 */
    {
        struct globus_l_gfs_posix_cksm_md5_cb_t * md5updt;

        rc = stat(filename, &stbuf);
        if (rc != 0 || ! S_ISREG(stbuf.st_mode) || (fd = open(filename,O_RDONLY)) < 0)
            return GLOBUS_FAILURE;

        md5updt = globus_malloc(sizeof( struct globus_l_gfs_posix_cksm_md5_cb_t));
        MD5_Init(&(md5updt->c));

        if (length < 0 || (offset + length) > stbuf.st_size) 
            length = stbuf.st_size - offset;

        lseek(fd, offset, SEEK_SET); 

        md5updt->op = op;
        md5updt->fd = fd;
        md5updt->blocksize = MAXBLOCSIZE4CKSM;
        md5updt->fsize = stbuf.st_size;
        md5updt->offset = offset;
        md5updt->length = length;
        md5updt->total_bytes = 0;
        globus_gridftp_server_get_update_interval(op, &md5updt->marker_freq);
        md5updt->t_lastmarker = time(NULL);


        result = globus_callback_register_oneshot( NULL,
                                                   NULL,
                                                   globus_l_gfs_posix_cksm_md5_cb,
                                                   md5updt);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_callback_register_oneshot", result);
            globus_panic(NULL, result, "oneshot failed, no recovery");
        }

    }
    return GLOBUS_SUCCESS;
}

/*************************************************************************
 *  command
 *  -------
 *  This interface function is called when the client sends a 'command'.
 *  commands are such things as mkdir, remdir, delete.  The complete
 *  enumeration is below.
 *
 *  To determine which command is being requested look at:
 *      cmd_info->command
 *
 *      GLOBUS_GFS_CMD_MKD = 1,
 *      GLOBUS_GFS_CMD_RMD,
 *      GLOBUS_GFS_CMD_DELE,
 *      GLOBUS_GFS_CMD_RNTO,
 *      GLOBUS_GFS_CMD_RNFR,
 *      GLOBUS_GFS_CMD_CKSM,
 *      GLOBUS_GFS_CMD_SITE_CHMOD,
 *      GLOBUS_GFS_CMD_SITE_DSI
 ************************************************************************/
static
void
globus_l_gfs_posix_command(
    globus_gfs_operation_t              op,
    globus_gfs_command_info_t *         cmd_info,
    void *                              user_arg)
{
    char *                              PathName;
    globus_l_gfs_posix_handle_t *       posix_handle;
    globus_result_t                     rc;
    struct utimbuf                      ubuf;
    GlobusGFSName(globus_l_gfs_posix_command);

    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    PathName=cmd_info->pathname;
    while (PathName[0] == '/' && PathName[1] == '/')
    {
        PathName++;
    }

    rc = GLOBUS_SUCCESS;
    switch(cmd_info->command)
    {
      case GLOBUS_GFS_CMD_MKD:
        (mkdir(PathName, 0777) == 0) || 
            (rc = GlobusGFSErrorSystemError("mkdir", errno)); 
        break;
      case GLOBUS_GFS_CMD_RMD:
        (rmdir(PathName) == 0) || 
            (rc = GlobusGFSErrorSystemError("rmdir", errno)); 
        break;
      case GLOBUS_GFS_CMD_DELE:
        (unlink(PathName) == 0) ||
            (rc = GlobusGFSErrorSystemError("unlink", errno)); 
        break;
      case GLOBUS_GFS_CMD_TRNC:
        (truncate(PathName, cmd_info->cksm_offset) == 0) ||
            (rc = GlobusGFSErrorSystemError("truncate", errno)); 
        break;
      case GLOBUS_GFS_CMD_SITE_RDEL:
        rc = globus_l_gfs_file_delete_dir(PathName);
        break;
      case GLOBUS_GFS_CMD_RNTO:
        (rename(cmd_info->rnfr_pathname, PathName) == 0) || 
            (rc = GlobusGFSErrorSystemError("rename", errno)); 
        break;
      case GLOBUS_GFS_CMD_SITE_CHMOD:
        (chmod(PathName, cmd_info->chmod_mode) == 0) ||
            (rc = GlobusGFSErrorSystemError("chmod", errno)); 
        break;
      case GLOBUS_GFS_CMD_SITE_CHGRP:
        (globus_l_gfs_posix_chgrp(PathName, cmd_info->chgrp_group) == 0) ||
            (rc = GlobusGFSErrorSystemError("chgrp", errno));
        break;
      case GLOBUS_GFS_CMD_SITE_UTIME:
        ubuf.modtime = cmd_info->utime_time;
        ubuf.actime = time(NULL);

        (utime(PathName, &ubuf) == 0) || 
            (rc = GlobusGFSErrorSystemError("utime", errno));
        break;
      case GLOBUS_GFS_CMD_SITE_SYMLINK:
        (symlink(cmd_info->from_pathname, cmd_info->pathname) == 0) || 
            (rc = GlobusGFSErrorSystemError("symlink", errno));
        break;
      case GLOBUS_GFS_CMD_CKSM:
        if (!strcmp(cmd_info->cksm_alg, "adler32") || 
            !strcmp(cmd_info->cksm_alg, "ADLER32"))
            rc = globus_l_gfs_posix_cksm_adler32(op, 
                                                 PathName);
        else if (!strcmp(cmd_info->cksm_alg, "md5") ||
                  !strcmp(cmd_info->cksm_alg, "MD5"))
            rc = globus_l_gfs_posix_cksm_md5(op,
                                             PathName,
                                             cmd_info->cksm_offset,
                                             cmd_info->cksm_length);
        else
            rc = GLOBUS_FAILURE;
        break;

      default:
        rc = GLOBUS_FAILURE;
        break;
    }

    if ( rc != GLOBUS_SUCCESS || cmd_info->command != GLOBUS_GFS_CMD_CKSM)
        globus_gridftp_server_finished_command(op, rc, NULL);
}

/* receive file from client */

static
void
globus_l_gfs_posix_write_to_storage(
    globus_l_gfs_posix_handle_t *      posix_handle);

static
void 
globus_l_gfs_posix_write_to_storage_cb(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_byte_t *                     buffer,
    globus_size_t                       nbytes,
    globus_off_t                        offset,
    globus_bool_t                       eof,
    void *                              user_arg)
{
    globus_off_t                        start_offset;
    globus_size_t                       bytes_written;
    globus_result_t                     rc; 
    globus_l_gfs_posix_handle_t *       posix_handle;
                                                                                                                                           
    GlobusGFSName(globus_l_gfs_posix_write_to_storage_cb);
    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    globus_mutex_lock(&posix_handle->mutex);
    rc = GLOBUS_SUCCESS;
    if (result != GLOBUS_SUCCESS)
    {
        rc = GlobusGFSErrorGeneric("call back fail");
        posix_handle->done = GLOBUS_TRUE;
    }
    else if (eof)
    {
        posix_handle->done = GLOBUS_TRUE;
    }

    if (nbytes > 0)
    {
        if (posix_handle->seekable)
        {
            start_offset = lseek(posix_handle->fd, offset, SEEK_SET);
        }

        if (posix_handle->seekable && start_offset != offset) 
        {
            rc = GlobusGFSErrorSystemError("lseek", errno);
            posix_handle->done = GLOBUS_TRUE;
        }
        else
        {
            bytes_written = write(posix_handle->fd, buffer, nbytes);
            if (bytes_written < nbytes) 
            {
                rc = GlobusGFSErrorSystemError("write", errno);
                posix_handle->done = GLOBUS_TRUE;
            }
            else
            {
                globus_gridftp_server_update_bytes_written(op, offset, nbytes);
            }
            if (nbytes != local_io_block_size)
            {
                 if (local_io_block_size != 0)
                 {
                      sprintf(err_msg,"receive %d blocks of size %d bytes\n",
                                      local_io_count,local_io_block_size);
                      globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
                 }
                 local_io_block_size = nbytes;
                 local_io_count=1;
            }
            else
            {
                 local_io_count++;
            }
        }
    }
    globus_free(buffer);

    posix_handle->outstanding--;
    if (! posix_handle->done)
    {
        globus_l_gfs_posix_write_to_storage(posix_handle);
    }
    else if (posix_handle->outstanding == 0) 
    {
        if (close(posix_handle->fd) == -1) 
        {
             rc = GlobusGFSErrorSystemError("close", errno);
        }
        sprintf(err_msg,"receive %d blocks of size %d bytes\n",
                        local_io_count,local_io_block_size);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
        local_io_count = 0;
        local_io_block_size = 0;

        globus_gridftp_server_finished_transfer(op, rc);
    }
    globus_mutex_unlock(&posix_handle->mutex);
}

static
void
globus_l_gfs_posix_write_to_storage(
    globus_l_gfs_posix_handle_t *      posix_handle)
{
    globus_byte_t *                     buffer;
    globus_result_t                     rc;

    GlobusGFSName(globus_l_gfs_posix_write_to_storage);
    globus_gridftp_server_get_optimal_concurrency(posix_handle->op,
                                                  &posix_handle->optimal_count);

    while (posix_handle->outstanding < posix_handle->optimal_count) 
    {
        buffer = globus_malloc(posix_handle->block_size);
        if (buffer == NULL)
        {
            rc = GlobusGFSErrorGeneric("fail to allocate buffer");
            globus_gridftp_server_finished_transfer(posix_handle->op, rc);
            return;
        }
        rc = globus_gridftp_server_register_read(posix_handle->op,
                                       buffer,
                                       posix_handle->block_size,
                                       globus_l_gfs_posix_write_to_storage_cb,
                                       posix_handle);
        if (rc != GLOBUS_SUCCESS)
        {
            rc = GlobusGFSErrorGeneric("globus_gridftp_server_register_read() fail");
            globus_gridftp_server_finished_transfer(posix_handle->op, rc);
            return;
        }
        posix_handle->outstanding++;
    }
    return; 
}

/*************************************************************************
 *  recv
 *  ----
 *  This interface function is called when the client requests that a
 *  file be transfered to the server.
 *
 *  To receive a file the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_read();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/
static
void
globus_l_gfs_posix_recv(
    globus_gfs_operation_t              op,
    globus_gfs_transfer_info_t *        transfer_info,
    void *                              user_arg)
{
    globus_l_gfs_posix_handle_t *      posix_handle;
    globus_result_t                     rc; 
    struct stat                         stat_buffer;
    char *filename;

    filename=NULL;

    GlobusGFSName(globus_l_gfs_posix_recv);

    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    posix_handle->pathname = transfer_info->pathname;
    while (posix_handle->pathname[0] == '/' && posix_handle->pathname[1] == '/')
    {
        posix_handle->pathname++;
    }

    posix_handle->op = op;
    posix_handle->outstanding = 0;
    posix_handle->done = GLOBUS_FALSE;
    globus_gridftp_server_get_block_size(op, &posix_handle->block_size); 

    globus_gridftp_server_get_write_range(posix_handle->op,
                                          &posix_handle->offset,
                                          &posix_handle->block_length);

    globus_gridftp_server_begin_transfer(posix_handle->op, 0, posix_handle);

/* 
   Calculate space usage of a xrootd space token. This is xrootd specific.
   None xrootd storage can still use it if XROOTD_CNSURL is not defined 
*/
    char *cns, *token, *tokenbuf[128], *key, *value, xattrs[1024], *xattrbuf[1024];
    long long spaceusage, spacequota;

    char ext_cmd[1024], *tfilename;
    FILE *F;

    cns = getenv("XROOTD_CNSURL");
    if (cns != NULL)
    {
        strcpy(xattrs, posix_handle->pathname);
        token = strtok_r(xattrs, "?", xattrbuf);
        token = strtok_r(NULL, "=", xattrbuf);
        token = strtok_r(NULL, "=", xattrbuf);
        sprintf(err_msg, "open() fail: quota exceeded for space token %s\n", token);

        strcat(cns, "/?oss.cgroup=");
        if (token == NULL)
            strcat(cns, "public");
        else
            strcat(cns, token);

        if (getxattr(cns, "xroot.space", xattrs, 128) > 0)
        {
            spaceusage = 0;
            spacequota = 0;
            token = strtok_r(xattrs, "&", xattrbuf);
            while (token != NULL)
            {
                 token = strtok_r(NULL, "&", xattrbuf);
                 if (token == NULL) break;
                 key = strtok_r(token, "=", tokenbuf);
                 value = strtok_r(NULL, "=", tokenbuf);
                 if (!strcmp(key,"oss.used"))
                 {
                     sscanf((const char*)value, "%lld", &spaceusage);
                 }
                 else if (!strcmp(key,"oss.quota"))
                 {
                     sscanf((const char*)value, "%lld", &spacequota);
                 }
            }
            if (spaceusage > spacequota) 
            {
                rc = GlobusGFSErrorGeneric(err_msg);
                globus_gridftp_server_finished_transfer(op, rc);
                return;
            }
        }
    }

    if ((tfilename = getenv("GRIDFTP_APPEND_XROOTD_CGI")) != NULL)  // transform filepath to be opened
    {
        strcpy(ext_cmd, tfilename);
        strcat(ext_cmd, " ");
        strcat(ext_cmd, posix_handle->pathname);

        F = popen(ext_cmd, "r");
        if (F) 
        {
            filename = malloc(sizeof(char)*1024);
            fscanf(F, "%s", filename);
            pclose(F);
        }
    }
/* end of XROOTD specfic code */
    
    if ( filename == NULL ) filename = posix_handle->pathname;
    if (stat(posix_handle->pathname, &stat_buffer) == 0)
    {
        posix_handle->fd = open(filename, O_WRONLY); /* |O_TRUNC);  */
    }
    else if (errno == ENOENT)
    {
        posix_handle->fd = open(filename, O_WRONLY|O_CREAT,
                                 S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);    
    }

    if (posix_handle->fd == -1)
    {
        rc = GlobusGFSErrorSystemError("open", errno);
        globus_gridftp_server_finished_transfer(op, rc);
    }

/*
 * /dev/null and /dev/zero are not seekable. They are used for memory-to-memory
 * performance test
 */
    posix_handle->seekable=1;
    if (! strcmp(posix_handle->pathname,"/dev/null"))
    {
        posix_handle->seekable=0;
    }

    globus_mutex_lock(&posix_handle->mutex);
    globus_l_gfs_posix_write_to_storage(posix_handle);
    globus_mutex_unlock(&posix_handle->mutex);
    return;
}

/* send files to client */

static
void
globus_l_gfs_posix_read_from_storage(
    globus_l_gfs_posix_handle_t *      posix_handle);

static
void
globus_l_gfs_posix_read_from_storage_cb(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_byte_t *                     buffer,
    globus_size_t                       nbytes,
    void *                              user_arg)
{
    GlobusGFSName(globus_l_gfs_posix_read_from_storage_cb);
    globus_l_gfs_posix_handle_t *      posix_handle;
 
    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    posix_handle->outstanding--;
    globus_free(buffer);
    globus_l_gfs_posix_read_from_storage(posix_handle);
}


static
void
globus_l_gfs_posix_read_from_storage(
    globus_l_gfs_posix_handle_t *      posix_handle)
{
    globus_byte_t *                     buffer;
    globus_size_t                       nbytes;
    globus_size_t                       read_length;
    globus_result_t                     rc;

    GlobusGFSName(globus_l_gfs_posix_read_from_storage);

    globus_mutex_lock(&posix_handle->mutex);
    while (posix_handle->outstanding < posix_handle->optimal_count &&
           ! posix_handle->done) 
    {
        buffer = globus_malloc(posix_handle->block_size);
        if (buffer == NULL)
        {
            rc = GlobusGFSErrorGeneric("fail to allocate buffer");
            globus_gridftp_server_finished_transfer(posix_handle->op, rc);
            return;
        }
/*
        if (posix_handle->seekable)
        {
            lseek(posix_handle->fd, posix_handle->offset, SEEK_SET);
        }
 */ 
        /* block_length == -1 indicates transferring data to until eof */
        if (posix_handle->block_length < 0 ||   
            posix_handle->block_length > posix_handle->block_size)
        {
            read_length = posix_handle->block_size;
        }
        else
        {
            read_length = posix_handle->block_length;
        }
 
        nbytes = read(posix_handle->fd, buffer, read_length);
        if (nbytes == 0)    /* eof */
        {
            posix_handle->done = GLOBUS_TRUE;
            sprintf(err_msg,"send %d blocks of size %d bytes\n",
                            local_io_count,local_io_block_size);
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
            local_io_count = 0;
            local_io_block_size = 0;
        }
        else
        {
            if (nbytes != local_io_block_size)
            {
                 if (local_io_block_size != 0)
                 {
                      sprintf(err_msg,"send %d blocks of size %d bytes\n",
                                      local_io_count,local_io_block_size);
                      globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
                 }
                 local_io_block_size = nbytes;
                 local_io_count=1;
            }
            else
            {
                 local_io_count++;
            }
        }
        if (! posix_handle->done) 
        {
            posix_handle->outstanding++;
            posix_handle->offset += nbytes;
            posix_handle->block_length -= nbytes;
            rc = globus_gridftp_server_register_write(posix_handle->op,
                                       buffer,
                                       nbytes,
                                       posix_handle->offset - nbytes,
                                       -1,
                                       globus_l_gfs_posix_read_from_storage_cb,
                                       posix_handle);
            if (rc != GLOBUS_SUCCESS)
            {
                rc = GlobusGFSErrorGeneric("globus_gridftp_server_register_write() fail");
                globus_gridftp_server_finished_transfer(posix_handle->op, rc);
            }
        }
    }
    globus_mutex_unlock(&posix_handle->mutex);
    if (posix_handle->outstanding == 0)
    {
        close(posix_handle->fd);
        globus_gridftp_server_finished_transfer(posix_handle->op, 
                                                GLOBUS_SUCCESS);
    }
    return;
}

/*************************************************************************
 *  send
 *  ----
 *  This interface function is called when the client requests to receive
 *  a file from the server.
 *
 *  To send a file to the client the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_write();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/
static
void
globus_l_gfs_posix_send(
    globus_gfs_operation_t              op,
    globus_gfs_transfer_info_t *        transfer_info,
    void *                              user_arg)
{
    globus_result_t                     rc;
    globus_l_gfs_posix_handle_t *       posix_handle;
    GlobusGFSName(globus_l_gfs_posix_send);

    posix_handle = (globus_l_gfs_posix_handle_t *) user_arg;

    posix_handle->pathname = transfer_info->pathname;
    while (posix_handle->pathname[0] == '/' && posix_handle->pathname[1] == '/')
    {
        posix_handle->pathname++;
    }

    posix_handle->op = op;
    posix_handle->outstanding = 0;
    posix_handle->done = GLOBUS_FALSE;
    globus_gridftp_server_get_block_size(op, &posix_handle->block_size);

    globus_gridftp_server_get_read_range(posix_handle->op,
                                         &posix_handle->offset,
                                         &posix_handle->block_length);

    globus_gridftp_server_begin_transfer(posix_handle->op, 0, posix_handle);
    posix_handle->fd = open(posix_handle->pathname, O_RDONLY);
    if (posix_handle->fd == -1)
    {
        rc = GlobusGFSErrorSystemError("open", errno);
        globus_gridftp_server_finished_transfer(op, rc);
    }

/*
 * /dev/null and /dev/zero are not seekable. They are used for memory-to-memory
 * performance test.
 */
    posix_handle->seekable=1;
    if (! strcmp(posix_handle->pathname,"/dev/zero"))
    {
        posix_handle->seekable=0;
    }
    else 
    {
        lseek(posix_handle->fd, posix_handle->offset, SEEK_SET);
    }

    globus_gridftp_server_get_optimal_concurrency(posix_handle->op,
                                                  &posix_handle->optimal_count);

    globus_l_gfs_posix_read_from_storage(posix_handle);
    return;
}

static
int
globus_l_gfs_posix_activate(void);

static
int
globus_l_gfs_posix_deactivate(void);

/*
 *  no need to change this
 */
static globus_gfs_storage_iface_t       globus_l_gfs_posix_dsi_iface = 
{
    GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING | GLOBUS_GFS_DSI_DESCRIPTOR_SENDER,
    globus_l_gfs_posix_start,
    globus_l_gfs_posix_destroy,
    NULL, /* list */
    globus_l_gfs_posix_send,
    globus_l_gfs_posix_recv,
    NULL, /* trev */
    NULL, /* active */
    NULL, /* passive */
    NULL, /* data destroy */
    globus_l_gfs_posix_command, 
    globus_l_gfs_posix_stat,
    NULL,
    NULL
};

/*
 *  no need to change this
 */
GlobusExtensionDefineModule(globus_gridftp_server_posix) =
{
    "globus_gridftp_server_posix",
    globus_l_gfs_posix_activate,
    globus_l_gfs_posix_deactivate,
    NULL,
    NULL,
    &local_version
};

/*
 *  no need to change this
 */
static
int
globus_l_gfs_posix_activate(void)
{
    globus_extension_registry_add(
        GLOBUS_GFS_DSI_REGISTRY,
        "posix",
        GlobusExtensionMyModule(globus_gridftp_server_posix),
        &globus_l_gfs_posix_dsi_iface);
    
    return 0;
}

/*
 *  no need to change this
 */
static
int
globus_l_gfs_posix_deactivate(void)
{
    globus_extension_registry_remove(
        GLOBUS_GFS_DSI_REGISTRY, "posix");

    return 0;
}
