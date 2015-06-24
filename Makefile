CC = gcc
CPP = g++
LD = gcc
AS = as
AR = ar

PRG = example

DISPMANX_CFLAGS=-mfpu=vfp -mfloat-abi=hard -march=armv6j
DISPMANX_LIBS=-L/opt/vc/lib -lbcm_host -lvcos -lvchiq_arm
DISPMANX_INCLUDES=-I/opt/vc/include -I/opt/vc/include/interface/vmcs_host/linux/ -I/opt/vc/include/interface/vcos/pthreads

LIBS = `sdl-config --libs` -lm  $(DISPMANX_LIBS)
# ./libbassmod.so
#LDFLAGS = -s -Wall -Wl
# -ggdb3
INCLUDES = $(DISPMANX_INCLUDES)
OPTIM = -ffast-math -funroll-loops -O3 -fomit-frame-pointer -fmessage-length=0
DEBUG = -O0 -ggdb
#CFLAGS = $(INCLUDES) $(OPTIM) -Wall
CFLAGS = $(INCLUDES) $(DEBUG) -Wall

OBJS =  raspberrypi.o main.o

all:	$(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(PRG)

clean:
	rm -f *.o $(PRG).map $(PRG)

%.o: %.cpp
	$(CPP) $(CFLAGS) -o $@ -c $<

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<
