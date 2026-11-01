SHELL = /bin/sh
MAIN_NAME=sls
CLIENT_NAME=slc
INC_PATH = -I./ -I../ -I./slscore -I./include
LIB_PATH =  -L ./lib
LIBRARY_FILE = -lpthread -lsrt
BIN_PATH = ./bin

DEBUG = -g
WARN  = -Wall -Wformat=2 -Wno-format-nonliteral -Wno-format-truncation -Wno-invalid-offsetof
OPT = -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDOPT = -pie -Wl,-z,relro,-z,now
CFLAGS += $(DEBUG) $(WARN) $(OPT)
LDFLAGS += $(LDOPT)

LOG_PATH = ./logs


OUTPUT_PATH = ./obj
OBJS = $(OUTPUT_PATH)/SLSLog.o \
	$(OUTPUT_PATH)/common.o\
	$(OUTPUT_PATH)/SLSStreamId.o\
	$(OUTPUT_PATH)/conf.o\
	$(OUTPUT_PATH)/SLSThread.o\
	$(OUTPUT_PATH)/SLSEpollThread.o\
	$(OUTPUT_PATH)/SLSManager.o\
	$(OUTPUT_PATH)/SLSGroup.o\
	$(OUTPUT_PATH)/SLSRole.o\
	$(OUTPUT_PATH)/SLSListener.o\
	$(OUTPUT_PATH)/SLSRoleList.o\
	$(OUTPUT_PATH)/SLSConnLimiter.o\
	$(OUTPUT_PATH)/SLSUserDb.o\
	$(OUTPUT_PATH)/SLSSrt.o\
	$(OUTPUT_PATH)/SLSPublisher.o\
	$(OUTPUT_PATH)/SLSPlayer.o\
	$(OUTPUT_PATH)/SLSRecycleArray.o\
	$(OUTPUT_PATH)/SLSMapData.o\
	$(OUTPUT_PATH)/SLSTsParser.o\
	$(OUTPUT_PATH)/SLSMapPublisher.o\
	$(OUTPUT_PATH)/SLSRelay.o\
	$(OUTPUT_PATH)/SLSPuller.o\
	$(OUTPUT_PATH)/SLSPusher.o\
	$(OUTPUT_PATH)/SLSRelayManager.o\
	$(OUTPUT_PATH)/SLSPullerManager.o\
	$(OUTPUT_PATH)/SLSPusherManager.o\
	$(OUTPUT_PATH)/SLSMapRelay.o\
	$(OUTPUT_PATH)/SLSClient.o\
	$(OUTPUT_PATH)/TCPRole.o\
	$(OUTPUT_PATH)/SLSArray.o\
	$(OUTPUT_PATH)/HttpClient.o\
	$(OUTPUT_PATH)/SLSSyncClock.o\
	$(OUTPUT_PATH)/TSFileTimeReader.o
	
CORE_PATH = slscore
COMMON_FILES = common.hpp

all: $(OBJS)
	mkdir -p ${LOG_PATH}
	mkdir -p ${OUTPUT_PATH}
	mkdir -p ${BIN_PATH}
	${CXX} -o ${BIN_PATH}/${MAIN_NAME}   srt-live-server.cpp $(OBJS) $(CFLAGS) $(INC_PATH) $(LIB_PATH) $(LDFLAGS) $(LIBRARY_FILE)
	${CXX} -o ${BIN_PATH}/${CLIENT_NAME} srt-live-client.cpp $(OBJS) $(CFLAGS) $(INC_PATH) $(LIB_PATH) $(LDFLAGS) $(LIBRARY_FILE)
	#******************************************************************************#
	#                          Build successful !                                  #
	#******************************************************************************#

$(OUTPUT_PATH)/%.o: ./$(CORE_PATH)/%.cpp | $(OUTPUT_PATH)
	${CXX} -c -MMD -MP $(CFLAGS) $(INC_PATH) $< -o $@

$(OUTPUT_PATH):
	mkdir -p $@

-include $(OBJS:.o=.d)

# Sanitizer build: separate object/binary dirs, no FORTIFY.
asan:
	$(MAKE) all OUTPUT_PATH=obj-asan BIN_PATH=bin-asan OPT='-O1 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fsanitize=address,undefined -fno-omit-frame-pointer -fPIE' LDOPT='-pie -fsanitize=address,undefined'

.PHONY: all asan clean

clean:
	rm -f $(OUTPUT_PATH)/*.o $(OUTPUT_PATH)/*.d
	rm -rf $(BIN_PATH)/*

