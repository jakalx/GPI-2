OFED_PATH = /usr
OFED_MIC_PATH = /opt/intel/mic/ofed/card/usr
CC = gcc
FORTRANC = gfortran
HAS_FORTRAN:=$(shell which $(FORTRANC))

CFLAGS += -Wall
CFLAGS += -D_GNU_SOURCE
CFLAGS += -O2

DBG_CFLAGS += -Wall
DBG_CFLAGS += -g
DBG_CFLAGS += -D_GNU_SOURCE
DBG_CFLAGS += -O0

MIC_CFLAGS += -Wall
MIC_CFLAGS += -D_GNU_SOURCE
MIC_CFLAGS += -O2
MIC_CFLAGS += -I$(OFED_MIC_PATH)/include
MIC_CFLAGS += -I../include
MIC_CFLAGS += -I.

INCLUDES += -I$(OFED_PATH)/include
INCLUDES += -I../include
INCLUDES += -I.