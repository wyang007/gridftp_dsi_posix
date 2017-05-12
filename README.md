globus_gridftp_server_posix.c

Auther: Wei Yang (Stanford Linear Accelerator Center, 2007)

Globus Gridftp 4.x Data Storage Interface module using POSIX IO

The following functions are copied from the original globus DSI
module for "file"

    globus_l_gfs_file_copy_stat()
    globus_l_gfs_file_destroy_stat()
    globus_l_gfs_file_partition_path()
    globus_l_gfs_posix_stat()
    globus_l_gfs_file_delete_dir()

How to compile:

This file should be compiled along with the globus 4.0.x source code.
Please copy this file to source-trees/gridftp/server/src/dsi_bones
and make adjustment to the Makefile. You may want to read the
README.txt file in that directory first. It will be compiled to a
shared library file libglobus_gridftp_server_posix_gcc32dbg.so.

How to use it with xrootd:                                           
                                                                     
This code does not use any Xrootd specific functions. So it should   
work with other types of storage in principle. However, we only      
tested it with xrootd. We have used it under vdt1.3.9, 1.6.1 and     
OSG 0.4.0, 0.6.                                                      
                                                                     
The idea is to overload the Posix IO functions by those provided in  
the xrootd Posix interface. To do it, modify VDT's gridftp start up
script to something like this:

#!/bin/sh

. /opt/vdt/setup.sh

XRDLIB="/path_to_xrootd_lib_dir"
if [ -z "$LD_LIBRARY_PATH" ]; then
    export LD_LIBRARY_PATH="$XRDLIB"
else
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:$XRDLIB"
fi
export LD_PRELOAD=$XRDLIB/libXrdPosixPreload.so
export XROOTD_VMP="host:1094:/path1=/path2"

exec /opt/vdt/globus/sbin/globus-gridftp-server -dsi posix

Note that the option in the last line (-dsi posix) will load
libglobus_gridftp_server_posix_gcc32dbg.so, So the .so should be in
the search path of LD_LIBRARY_PATH

Please refer to src/XrdPosix/README for description on how to use
environment variable XROOTD_VMP
