include makefile_header
# include additional makefile headers here

# add needed cflags here
DSI_CFLAGS=$(GLOBUS_CFLAGS)

# add needed includes here
DSI_INCLUDES=$(GLOBUS_INCLUDES)

# added needed ldflags here
DSI_LDFLAGS=$(GLOBUS_LDFLAGS)

# add needed libraries here
DSI_LIBS=

FLAVOR=

globus_gridftp_server_posix.o:
	$(GLOBUS_CC) $(DSI_CFLAGS) $(DSI_INCLUDES) \
		-shared -o libglobus_gridftp_server_posix_$(FLAVOR).so \
		globus_gridftp_server_posix.c \
		$(DSI_LDFLAGS) $(DSI_LIBS)

install:
	cp -f libglobus_gridftp_server_posix_$(FLAVOR).so $(GLOBUS_LOCATION)/ 
lib

clean:
	rm -f *.so
