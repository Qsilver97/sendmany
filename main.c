

//#define TESTNET
#define FROMMAIN

#ifdef TESTNET
#define DEFAULT_NODE_PORT 31842
#define DEFAULT_NODE_IP ((char *)"193.135.9.63")
#define TICKOFFSET 3
#else
#define DEFAULT_NODE_PORT 21841
#define DEFAULT_NODE_IP ((char *)"62.113.194.217")
#define TICKOFFSET 3
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "utlist.h"

#include "K12AndKeyUtil.h"

#include "qdefines.h"
#include "qstructs.h"
#include "qkeys.c"
#include "qhelpers.c"
uint32_t LATEST_UTIME;
int32_t FANDEPTH,INITIAL_TICK,LATEST_TICK = 1;    // start at nonzero value to trigger initial requests
pthread_mutex_t txq_mutex,txq_sendmutex,conn_mutex;

// C code "linked" in by #include
#include "qtime.c"
#include "qconn.c"
#include "qserver.c"

#include "qcmds.c"
#include "qtx.c"
#include "qthreads.c"
#include "qpeers.c"
#include "qfanout.c"
#include "qsendmany.c"
#include "qtests.c"

#ifndef HAVEMAIN
int main(int argc, const char * argv[])
{
    //qserver();

    FILE *fp;
    int32_t i,len,autogenflag = 0;
    char *origseed,*argstr,line[512];
    uint64_t tmp;
    uint32_t seed;
    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&conn_mutex,NULL);
    pthread_mutex_init(&txq_mutex,NULL);
    pthread_mutex_init(&txq_sendmutex,NULL);
    devurandom((uint8_t *)&seed,sizeof(seed));
    srand(seed);
    txq_HASHSIZE = MAXFANS * 3 + 1;
    Balances = (struct balancetick *)calloc(txq_HASHSIZE,sizeof(*Balances));
    timebasedepoch(&i,&i);
    printf("LATEST_TICK.%d\n",LATEST_TICK);

    uint8_t ipbytes[6][4];
    pthread_t txq_peerloop_thread;
#ifdef TESTNET
    ipaddr2ipbytes("193.135.9.63",ipbytes[0]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[0]);
    ipaddr2ipbytes("194.158.200.8",ipbytes[1]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[1]);
    ipaddr2ipbytes("91.210.226.133",ipbytes[2]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[2]);
    ipaddr2ipbytes("91.210.226.146",ipbytes[3]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[3]);
    ipaddr2ipbytes("46.59.23.186",ipbytes[4]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[4]);
    ipaddr2ipbytes("93.170.237.216",ipbytes[5]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[5]);
#else
    ipaddr2ipbytes(DEFAULT_NODE_IP,ipbytes[0]);
    pthread_create(&txq_peerloop_thread,NULL,&txq_peerloop,ipbytes[0]);
    pthread_t initpeers_thread;
    pthread_create(&initpeers_thread,NULL,&initpeers,0);
#endif
    pthread_t txq_loop_thread;
    pthread_create(&txq_loop_thread,NULL,&txq_loop,0);
    if ( argc == 3 )
    {
        origseed = (char *)argv[1];
        argstr = (char *)argv[2];
    }
    else if ( argc == 2 )
    {
        origseed = (char *)"seed";
        argstr = (char *)argv[1];
    }
    else
    {
        origseed = (char *)"seed";
        argstr = "25";
        printf("usage:\n%s <seed> <csvname>\n%s <csvname>\nseed can be actual 55 char seed or filename with seed\ncsvname can be filename of payments CSV or a number for N test payments, using 0 for N will generate random N above 10000\nIf you use the name for seed of 'autogen', a random seed will automatically be generated and used.",argv[0],argv[0]);
#ifdef __LINUX__
        return(0);
#endif
    }
    if ( strcmp(origseed,"autogen") == 0 )
    {
        autogenflag = 1;
        for (i=0; i<55; i++)
        {
            devurandom((uint8_t *)&tmp,sizeof(tmp));    // slight bias for (2**64-1) % 26 and below
            line[i] = (tmp % 26) + 'a';
        }
        line[55] = 0;
        origseed = line;
        printf("autogenerated seed will be stored in file called seed in sendmany/addr.tick dir\n");
    }
    else if ( strlen(origseed) != 55 )
    {
        printf("use (%s) as seed filename\n",origseed);
        if ( (fp= fopen(origseed,"r")) != 0 )
        {
            if ( fgets(line,sizeof(line),fp) != 0 )
            {
                len = (int32_t)strlen(line);
                if ( len == 56 )
                {
                    line[55] = 0;
                    len = 55;
                }
                if ( len != 55 )
                {
                    printf("invalid seed len.%d\n",len);
                    return(-1);
                }
                origseed = line;
            }
            fclose(fp);
        }
    }
    sendmany(origseed,argstr,autogenflag);
    return(0);
}
#endif
