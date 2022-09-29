/*
 * Copyright (c) 2014-2016 IBM Corporation.
 * All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of the <organization> nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
// clang-format off
#include "oslmic.h"
#include "lmic.h"
// clang-format on
#include "debug.h"
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//////////////////////////////////////////////////
// CONFIGURATION (FOR APPLICATION CALLBACKS BELOW)
//////////////////////////////////////////////////

// application router ID (LSBF)
static const u1_t APPEUI[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// unique device ID (LSBF)
// static const u1_t DEVEUI[8]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
// 0x00 };
static u1_t DEVEUI[8];

// device-specific AES key (derived from device EUI)
// static const u1_t DEVKEY[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
// 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static u1_t DEVKEY[16];

static const u1_t lorawan_tx_uninitialized[] = "uninitialized";
static const u1_t lorawan_tx_joining[] = "joining";
static const u1_t lorawan_tx_joined[] = "joined";
static const u1_t lorawan_tx_joinfailed[] = "join failed";
static const u1_t lorawan_tx_rejoinfailed[] = "rejoin failed";
static const u1_t lorawan_tx_linkdead[] = "link dead";

redisContext *c;
redisReply *reply;

uint8_t ongoing_tx;
uint8_t join_number;
uint8_t use_lorabridge_gw;
uint8_t lorawan_link_status; // Boolean: 0 - Link dead, 1 - Link alive

//////////////////////////////////////////////////
// INITIALIZE REDIS CLIENT
//////////////////////////////////////////////////

void init_redis() {

    //   const char *hostname =  "127.0.0.1";
    const char *hostname = "redis";

    int port = 6379;

    struct timeval timeout = {1, 500000}; // 1.5 seconds
    // if (isunix) {
    //     c = redisConnectUnixWithTimeout(hostname, timeout);
    // } else {
    c = redisConnectWithTimeout(hostname, port, timeout);
    //}
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
}

//////////////////////////////////////////////////
// UI STATUS UPDATE FUNCTIONS
//////////////////////////////////////////////////

void update_ui_status(const u1_t *new_status) {

    if (c != NULL) {

        reply = redisCommand(c, "SET txstatus %s", new_status);

        if (c == NULL || c->err) {
            printf("Update of UI Status via redis command failed");
        }
    }
}

//////////////////////////////////////////////////
// APPLICATION CALLBACKS
//////////////////////////////////////////////////

// provide application router ID (8 bytes, LSBF)
void os_getArtEui(u1_t *buf) { memcpy(buf, APPEUI, 8); }

// provide device ID (8 bytes, LSBF)
void os_getDevEui(u1_t *buf) { memcpy(buf, DEVEUI, 8); }

// provide device key (16 bytes)
void os_getDevKey(u1_t *buf) { memcpy(buf, DEVKEY, 16); }

//////////////////////////////////////////////////
// MAIN - INITIALIZATION AND STARTUP
//////////////////////////////////////////////////

// initial job
static void initfunc(osjob_t *j) {
    // reset MAC state
    LMIC_reset();
    // Use lorabridge forwarder: yes = 1, no = 0
    LMIC_setLoRaBridgeJoinChannels(use_lorabridge_gw);
    // start joining
    LMIC_startJoining();
    // init done - onEvent() callback will be invoked...
    join_number = 1;
}

//////////////////////////////////////////////////
// REDIS QUERY
//////////////////////////////////////////////////

redisReply *fetch_redis_string(char *ieee, char *hash) {
    char command[] = "GETDEL lorabridge:device:%s:message:%s";
    char buf[strlen(command) + strlen(ieee) + strlen(hash) + 1];
    sprintf(buf, command, ieee, hash);
    reply = redisCommand(c, buf);

    if (reply->type == REDIS_REPLY_STRING) {
        return reply;
    } else {
        return NULL;
    }
}

redisReply *fetch_redis_queues() {

    reply = redisCommand(c, "SMEMBERS lorabridge:device:index");

    if (reply->type == REDIS_REPLY_ARRAY) {
        return reply;
    } else {
        return NULL;
    }
}

redisReply *fetch_redis_message(char *ieee) {
    char command[] = "ZPOPMIN lorabridge:queue:%s";
    char buf[strlen(command) + strlen(ieee) + 1];
    sprintf(buf, command, ieee);
    reply = redisCommand(c, buf);

    if (reply->type == REDIS_REPLY_ARRAY) {
        return reply;
    } else {
        return NULL;
    }
}

//////////////////////////////////////////////////
// UTILITY JOB
//////////////////////////////////////////////////

static osjob_t reportjob;

// report sensor value every minute
static void reportfunc(osjob_t *j) {

    // Check if link is alive?

    // If not, schedule a short heartbeat packet once per minute (not to congest LMIC at high SFs)

    if(lorawan_link_status == 0) {

        printf("Scheduling a lorabridge heartbeat packet...\n");

        memset(LMIC.frame,0,sizeof(LMIC.frame));

        LMIC.frame[0] = 'h';

        LMIC_setTxData2(1, LMIC.frame, 1, 0); // (port 1, 2 bytes, unconfirmed)

        os_setTimedCallback(j, os_getTime() + sec2osticks(60), reportfunc);

        return;
    }

    // Otherwise, prepare and schedule data for transmission

    // printf("Fetching a redis string...\n");

    time_t rawtime;
    struct tm *timeinfo;
    redisReply *queues = fetch_redis_queues();
    for (int i = 0; i < queues->elements; i++) {
        redisReply *msg = fetch_redis_message(queues->element[i]->str);
        if (msg->elements == 0) {
            continue;
        }

        redisReply *tx_string = fetch_redis_string(queues->element[i]->str, msg->element[0]->str);

        if (tx_string != NULL && ongoing_tx == 0) {
            printf(
                "\nScheduling a lorabridge_data string...\n"); //:%s\n",
                                                               //: tx_string->str);

            // memset(LMIC.frame,0,strlen(LMIC.frame));
            memcpy(LMIC.frame, tx_string->str, tx_string->len);

            printf("String copied to LMIC buffer\n");
            // LMIC_setDrTxpow(DR_SF12, 14);

            LMIC_setTxData2(1, LMIC.frame, tx_string->len,
                            0); // (port 1, 2 bytes, unconfirmed)
            // LMIC_setTxData2(1, LMIC.frame, 63, 0);

            ongoing_tx = 1;

            printf("String sent to LoRaWAN TX\n");

            time(&rawtime);
            timeinfo = localtime(&rawtime);
            printf("Timestamp: %s\n\n", asctime(timeinfo));
        }
        // else {
        //   printf("No lorabridge_data strings found...\n");
        // }

        fflush(stdout);
    }

    // reschedule job in 60 seconds
    os_setTimedCallback(j, os_getTime() + sec2osticks(5), reportfunc);
}

int HexStringToBytes(const char *hexStr, unsigned char *output,
                     unsigned int *outputLen) {
    size_t len = strlen(hexStr);
    if (len % 2 != 0) {
        return -1;
    }
    size_t finalLen = len / 2;
    *outputLen = finalLen;
    for (size_t inIdx = 0, outIdx = 0; outIdx < finalLen;
         inIdx += 2, outIdx++) {
        if ((hexStr[inIdx] - 48) <= 9 && (hexStr[inIdx + 1] - 48) <= 9) {
            goto convert;
        } else {
            if ((hexStr[inIdx] - 65) <= 5 && (hexStr[inIdx + 1] - 65) <= 5) {
                goto convert;
            } else {
                *outputLen = 0;
                return -1;
            }
        }
    convert:
        output[outIdx] = (hexStr[inIdx] % 32 + 9) % 25 * 16 +
                         (hexStr[inIdx + 1] % 32 + 9) % 25;
    }
    // do not treat output as string, but as byte array (no string termination
    // \0) output[finalLen] = '\0';
    return 0;
}

void ReverseArray(u1_t arr[], int size) {
    for (int i = 0; i < size / 2; i++) {
        u1_t temp = arr[i];
        arr[i] = arr[size - 1 - i];
        arr[size - 1 - i] = temp;
    }
}

// application entry point
int main() {

    osjob_t initjob;

    update_ui_status(lorawan_tx_uninitialized);

    if (!getenv("DEV_EUI") || !getenv("DEV_KEY") || !getenv("USE_LB_GW")) {
        fprintf(stderr, "environment variables not found.\n");
        exit(1);
    }

    uint8_t buf_size = 0;

    // get string DEV_EUI size
    buf_size = snprintf(DEVEUI, 8, "%s", getenv("DEV_EUI"));

    // read input is twice the size of the hex bytes
    // e.g. '01' (char len = 2) will result in 0x01 (char len = 1)
    if (buf_size != 16) {
        fprintf(
            stderr,
            "Illegal DEV_EUI length. Expected: 16, Got: %d bytes. Aborting\n",
            buf_size);
        exit(1);
    }

    // get string DEV_KEY size
    buf_size = snprintf(DEVKEY, 16, "%s", getenv("DEV_KEY"));

    if (buf_size != 32) {
        fprintf(
            stderr,
            "Illegal DEV_KEY length. Expected: 32, Got: %d bytes. Aborting\n",
            buf_size);
        exit(1);
    }

    unsigned int alen;
    unsigned int blen;
    HexStringToBytes(getenv("DEV_EUI"), DEVEUI, &alen);
    HexStringToBytes(getenv("DEV_KEY"), DEVKEY, &blen);
    // why is the endianness of DEVEUI and DEVKEY different...? see comment of
    // os_getDevEui and os_getDevKey above
    ReverseArray(DEVEUI, 8);
    //  ReverseArray(DEVKEY, 16);
    //  printf("%d\n",alen);
    //  printf("%d\n",blen);


    use_lorabridge_gw = 1;

    use_lorabridge_gw = atoi(getenv("USE_LB_GW"));

    printf("DEBUG: use lorabridge gw: %d\n", use_lorabridge_gw);

    if(use_lorabridge_gw != 0 && use_lorabridge_gw != 1) {
        fprintf(
            stderr,
            "Illegal USE_LB_GW value. Expected: 1/0, Got: %d. Aborting\n",
            use_lorabridge_gw);
        exit(1);
    }

    ongoing_tx = 0;

    lorawan_link_status = 1;

    // printf(DEVEUI);
    // printf("%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",DEVEUI[0],DEVEUI[1],DEVEUI[2],DEVEUI[3],DEVEUI[4],DEVEUI[5],DEVEUI[6],DEVEUI[7]);
    // printf("\n");
    // for(int i=0; i<16; i++){
    // 	  printf("%02x:", DEVKEY[i]);
    // }
    // printf("\n");
    // fflush(stdout);
    // initialize runtime env
    os_init();
    // initialize redis
    init_redis();
    // initialize debug library
    debug_init();    
    // setup initial job
    os_setCallback(&initjob, initfunc);
    // update ui indicators
    update_ui_status(lorawan_tx_joining);
    // execute scheduled jobs and events
    os_runloop();
    // (not reached)
    return 0;
}

//////////////////////////////////////////////////
// LMIC EVENT CALLBACK
//////////////////////////////////////////////////

void onEvent(ev_t ev) {
    debug_event(ev);

    switch (ev) {

    case EV_JOINING:

        printf("DEBUG: %d. joining attempt\n", join_number);

        fflush(stdout);

        join_number += 1;

        break;

    case EV_JOIN_FAILED:

        printf("DEBUG: Join failed!\n");

        fflush(stdout);

        update_ui_status(lorawan_tx_joinfailed);

        break;

    case EV_REJOIN_FAILED:

        fflush(stdout);

        update_ui_status(lorawan_tx_rejoinfailed);

        break;

    case EV_LINK_DEAD:

        printf("DEBUG: Link dead signal issued!\n");

        fflush(stdout);

        update_ui_status(lorawan_tx_linkdead);

        lorawan_link_status = 0;

        break;

    case EV_LINK_ALIVE:

        printf("DEBUG: Link alive signal issued!\n");

        fflush(stdout);

        update_ui_status(lorawan_tx_joined);

        lorawan_link_status = 1;

        break;

        // network joined, session established
    case EV_JOINED:
        debug_val("netid = ", LMIC.netid);

        printf("ev_joined");
        fflush(stdout);
        ongoing_tx = 0;
        lorawan_link_status = 1;
        join_number = 1;

        update_ui_status(lorawan_tx_joined);
        // printf("test");
        fflush(stdout);

        reportfunc(&reportjob);

        break;

        // scheduled data sent (optionally data received)
    case EV_TXCOMPLETE:

        ongoing_tx = 0;

        if (LMIC.dataLen) { // data received in rx slot after tx
            debug_buf(LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
        }

        break;
    }
}
