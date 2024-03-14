


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "utlist.h"
#include "uthash.h"

#ifndef FROMMAIN
#define MAX_INPUT_SIZE 1024
#ifdef TESTNET
#define DEFAULT_NODE_PORT 31841
#define DEFAULT_NODE_IP ((char *)"193.135.9.63")
#define TICKOFFSET 3
#else
#define DEFAULT_NODE_PORT 21841
#define DEFAULT_NODE_IP ((char *)"185.70.186.149")
#define TICKOFFSET 10
#endif

int32_t LATEST_TICK,INITIAL_TICK;
uint32_t LATEST_UTIME;
#include "K12AndKeyUtil.h"

#endif

#include "qdefines.h"
#include "qstructs.h"

#ifndef FROMMAIN
#include "qkeys.c"
#include "qhelpers.c"
#include "qconn.c"
#include "qtime.c"
#endif

const char *Peers[] =
{
#ifdef TESTNET
    "193.135.9.63"
#else
    "45.135.201.80",
    "31.214.243.32",
    "185.117.0.116",
    "88.198.25.67",
    "136.243.43.230",
    "31.214.243.25",
    "144.76.237.194",
    "136.243.36.28",
    "148.251.185.19",
    "5.199.134.150",
#endif
};

#define MAXPEERS 128
#define MAXADDRESSES 500001
#define MAXTICKS 150000
#define MSG_COPY        040000
#define ADDRSCAN_DEPTH 5000
#define QUORUM_FETCHWT 100
#define TX_FETCHWT 300

char Newpeers[64][16];
uint32_t EXITFLAG,PROGRESSTIME;
int32_t EPOCH,VALIDATED_TICK,HAVE_TXTICK,Numnewpeers,Numpeers,Numsubs,Numaddrs;
pthread_mutex_t addpeer_mutex,txid_mutex,balancechange_mutex;
struct brequest *BALANCEQ;

struct qpeer
{
    CurrentTickInfo info;
    char ipaddr[16];
    uint8_t packet[MAX_INPUT_SIZE*2];
    int32_t packetlen;
} Peertickinfo[MAXPEERS];

struct qubictx
{
    UT_hash_handle hh;
    uint8_t txid[32];
    int32_t txlen;
    uint8_t txdata[];
} *TXIDS;

struct RAM_Quorum
{
    Tick Quorum[NUMBER_OF_COMPUTORS];
    uint8_t qchain[32],spectrum[32];
    int32_t validated,count,pending,needtx;
} *RAMQ;

struct subscriber
{
    uint64_t peersflag,key;
    int32_t respid,numpubkeys,errs,flag,last_validated;
    uint8_t *pubkeys;
} *SUBS;

struct addrhash
{
    uint8_t merkleroot[32];
    struct Entity entity;
    int32_t tick,flushtick;
    uint64_t flushmerkle64,prevsent,prevrecv;
} *Addresses;

int32_t flushaddress(struct addrhash *ap,int64_t *sentp,int64_t *recvp,long *fposp)
{
    FILE *fp;
    int32_t firstflag,retval = 0;
    char fname[512],addr[64];
    *sentp = *recvp = 0;
    *fposp = -1;
    if ( ap->flushtick < ap->tick || *(uint64_t *)ap->merkleroot != ap->flushmerkle64 || ap->prevsent != ap->entity.outgoingAmount || ap->prevrecv != ap->entity.incomingAmount )
    {
        pubkey2addr(ap->entity.publicKey,addr);
        sprintf(fname,"addrs%c%s",dir_delim(),addr);
        if ( (fp= fopen(fname,"rb+")) != 0 )
            fseek(fp,0,SEEK_END);
        else fp = fopen(fname,"wb");
        if ( fp != 0 )
        {
            *fposp = ftell(fp);
            fwrite(ap,1,sizeof(ap->entity)+40,fp);
            fclose(fp);
            if ( ap->flushtick == 0 )
                firstflag = 1;
            else firstflag = 0;
            ap->flushtick = ap->tick;
            ap->flushmerkle64 = *(uint64_t *)ap->merkleroot;
            if ( ap->prevrecv != ap->entity.incomingAmount )
            {
                if ( firstflag == 0 )
                {
                    retval |= 2;
                    *recvp = (ap->entity.incomingAmount - ap->prevrecv);
                    printf("%s received %s\n",addr,amountstr(ap->entity.incomingAmount - ap->prevrecv));
                }
                ap->prevrecv = ap->entity.incomingAmount;
            }
            if ( ap->prevsent != ap->entity.outgoingAmount )
            {
                if ( firstflag == 0 )
                {
                    retval |= 4;
                    *sentp = (ap->entity.outgoingAmount - ap->prevsent);
                    printf("%s sent %s\n",addr,amountstr(ap->entity.outgoingAmount - ap->prevsent));
                }
                ap->prevsent = ap->entity.outgoingAmount;
            }
            return(retval | 1);
        }
    }
    return(0);
}

struct addrhash *Addresshash(uint8_t pubkey[32],uint32_t utime)
{
    uint64_t hashi;
    int32_t i;
    uint8_t zero[32];
    struct addrhash *ap;
    hashi = *(uint64_t *)&pubkey[8] % MAXADDRESSES;
    memset(zero,0,sizeof(zero));
    for (i=0; i<MAXADDRESSES; i++)
    {
        ap = &Addresses[(hashi + i) % MAXADDRESSES];
        if ( memcmp(ap->entity.publicKey,pubkey,32) == 0 )
            return(ap);
        if ( ap->tick == 0 )
        {
            memcpy(ap->entity.publicKey,pubkey,32);
            ap->tick = 1;
            return(ap);
        }
    }
    printf("hash table full\n");
    return(0);
}

struct subscriber *Subscriberget(uint64_t key,uint32_t utime)
{
    int32_t i,n;
    FILE *fp;
    char fname[512],keystr[20],addr[64];
    struct subscriber *sp;
    for (i=0; i<Numsubs; i++)
        if ( SUBS[i].key == key )
            return(&SUBS[i]);
    SUBS = realloc(SUBS,sizeof(*SUBS) * (Numsubs+1));
    sp = &SUBS[Numsubs++];
    memset(sp,0,sizeof(*sp));
    sp->key = key;
    sp->last_validated = LATEST_TICK;
    byteToHex((uint8_t *)&key,keystr,sizeof(key));
    sprintf(fname,"subs%c%s",dir_delim(),keystr);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        fseek(fp,0,SEEK_END);
        if ( (ftell(fp) % 32) == 0 )
        {
            n = (int32_t)ftell(fp) / 32;
            rewind(fp);
            sp->pubkeys = calloc(n,32);
            fread(sp->pubkeys,n,32,fp);
            sp->numpubkeys = n;
            for (i=0; i<n; i++)
            {
                pubkey2addr(&sp->pubkeys[i*32],addr);
                Addresshash(&sp->pubkeys[i*32],utime);
                printf("%s ",addr);
            }
            printf("numaddrs.%d for %s\n",n,keystr);
        }
        fclose(fp);
    }
    return(sp);
}

int32_t SubscriberAddr(struct subscriber *sp,struct addrhash *ap)
{
    int32_t i;
    FILE *fp;
    char fname[512],keystr[20];
    for (i=0; i<sp->numpubkeys; i++)
        if ( memcmp(sp->pubkeys + i*32,ap->entity.publicKey,32) == 0 )
            return(0);
    sp->pubkeys = realloc(sp->pubkeys,32 * (sp->numpubkeys+1));
    memcpy(sp->pubkeys + sp->numpubkeys*32,ap->entity.publicKey,32);
    sp->numpubkeys++;
    byteToHex((uint8_t *)&sp->key,keystr,sizeof(sp->key));
    sprintf(fname,"subs%c%s",dir_delim(),keystr);
    if ( (fp= fopen(fname,"wb")) != 0 )
    {
        fwrite(sp->pubkeys,sp->numpubkeys,32,fp);
        fclose(fp);
    }
    return(1);
}

/*
void makesubslist(const char *fname)
{
    char cmd[1024];
    sprintf(cmd,"ls -w 16 subs > %s",fname);
    system(cmd);
}

void Subscriberinit(void)
{
    FILE *fp,*subfp;
    int32_t n = 0;
    char line[512],fname[512];
    makesubslist("subs.list");
    if ( (fp= fopen("subs.list","r")) != 0 )
    {
        while ( fgets(line,sizeof(line),fp) != 0 )
        {
            line[strlen(line)-1] = 0;
            printf("sub.%d %s\n",n,line);
            sprintf(fname,"subs%c%s",dir_delim(),line);
            if ( (subfp= fopen(fname,"rb")) != 0 )
            {
            }
            n++;
        }
        fclose(fp);
    }
}*/

void _qubictxadd(uint8_t txid[32],uint8_t *txdata,int32_t txlen)
{
    struct qubictx *qtx;
    qtx = (struct qubictx *)calloc(1,sizeof(*qtx) + txlen);
    memcpy(qtx->txid,txid,sizeof(qtx->txid));
    qtx->txlen = txlen;
    memcpy(qtx->txdata,txdata,txlen);
    HASH_ADD_KEYPTR(hh,TXIDS,qtx->txid,sizeof(qtx->txid),qtx);
}

struct qubictx *qubictxhash(uint8_t txid[32],uint8_t *txdata,int32_t txlen)
{
    struct qubictx *qtx;
    pthread_mutex_lock(&txid_mutex);
    HASH_FIND(hh,TXIDS,txid,sizeof(qtx->txid),qtx);
    if ( qtx == 0 && txdata != 0 && txlen >= sizeof(Transaction) )
    {
        _qubictxadd(txid,txdata,txlen);
        HASH_FIND(hh,TXIDS,txid,sizeof(qtx->txid),qtx);
        if ( qtx == 0 )
            printf("FATAL HASH TABLE ERROR\n");
    }
    pthread_mutex_unlock(&txid_mutex);
    return(qtx);
}

void qpurge(int32_t tick)
{
    char fname[512];
    sprintf(fname,"epochs%c%d%c%d.T",dir_delim(),EPOCH,dir_delim(),tick);
    deletefile(fname);
    sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),tick);
    deletefile(fname);
    sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
    deletefile(fname);
    if ( VALIDATED_TICK >= tick )
        VALIDATED_TICK = tick - 1;
    if ( HAVE_TXTICK >= tick )
        HAVE_TXTICK = tick - 1;
}

void qchaincalc(uint8_t prevhash[32],uint8_t qchain[32],Tick T)
{
    uint8_t digest[32];
    char hexstr[65];
    Qchain Q;
    memset(&Q,0,sizeof(Q));
    Q.epoch = T.epoch;
    Q.tick = T.tick;
    Q.millisecond = T.millisecond;
    Q.second = T.second;
    Q.minute = T.minute;
    Q.hour = T.hour;
    Q.day = T.day;
    Q.month = T.month;
    Q.year = T.year;
    Q.prevResourceTestingDigest = T.prevResourceTestingDigest;
    memcpy(Q.prevSpectrumDigest,T.prevSpectrumDigest,sizeof(Q.prevSpectrumDigest));
    memcpy(Q.prevUniverseDigest,T.prevUniverseDigest,sizeof(Q.prevUniverseDigest));
    memcpy(Q.prevComputerDigest,T.prevComputerDigest,sizeof(Q.prevComputerDigest));
    memcpy(Q.transactionDigest,T.transactionDigest,sizeof(Q.transactionDigest));
    memcpy(Q.prevqchain,prevhash,sizeof(Q.prevqchain));
    KangarooTwelve((uint8_t *)&Q,sizeof(Q),digest,32);
    byteToHex(digest,hexstr,sizeof(digest));
    if ( (T.tick % 1000) == 0 )
        printf("qchain.%-6d %s\n",T.tick,hexstr);
    memcpy(qchain,digest,sizeof(digest));
}

void incr_VALIDATE_TICK(int32_t tick,Tick T)
{
    int32_t offset;
    uint8_t prevhash[32];
    offset = (tick - INITIAL_TICK);
    if ( offset >= 0 && offset < MAXTICKS )
    {
        if ( offset == 0 )
            memset(prevhash,0,sizeof(prevhash));
        else
        {
            memcpy(RAMQ[offset-1].spectrum,T.prevSpectrumDigest,sizeof(prevhash));
            memcpy(prevhash,RAMQ[offset-1].qchain,sizeof(prevhash));
        }
        qchaincalc(prevhash,RAMQ[offset].qchain,T);
    }
    VALIDATED_TICK = tick;
    PROGRESSTIME = LATEST_UTIME;
    EXITFLAG = 0;
}

int32_t update_validated(void)
{
    Tick T;
    TickData TD;
    uint8_t zeros[32],digest[32];
    FILE *fp;
    int32_t tick,n,haveQ=0,deleteflag,missed = 0;
    char fname[512];
    n = 0;
    
    for (tick=VALIDATED_TICK!=0?VALIDATED_TICK:INITIAL_TICK; tick<LATEST_TICK && missed<1000; tick++)
    {
        n++;
        haveQ = 0;
        //printf("validate %d missed.%d\n",tick,missed);
        sprintf(fname,"epochs%c%d%c%d.T",dir_delim(),EPOCH,dir_delim(),tick);
        if ( (fp= fopen(fname,"rb")) != 0 )
        {
            fclose(fp);
            sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),tick);
            if ( (fp= fopen(fname,"rb")) != 0 )
            {
                while ( fread(&T,1,sizeof(T),fp) == sizeof(T) )
                {
                    if ( T.tick == tick )
                    {
                        haveQ = 1;
                        break;
                    }
                }
                fclose(fp);
                if ( haveQ != 0 )
                {
                    //printf("skip VALIDATED_TICK.%d\n",tick);
                    incr_VALIDATE_TICK(tick,T);
                    continue;
                }
            }
            //printf("haveT.%d tick.%d even with Qfile\n",haveT,tick);
        }
        sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),tick);
        if ( (fp= fopen(fname,"rb")) != 0 )
        {
            while ( fread(&T,1,sizeof(T),fp) == sizeof(T) )
            {
                if ( T.tick == tick )
                {
                    haveQ = 1;
                    break;
                }
            }
            fclose(fp);
            deleteflag = 0;
            sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
            if ( (fp= fopen(fname,"rb")) != 0 )
            {
                fseek(fp,0,SEEK_END);
                if ( ftell(fp) == 32 )
                    memset(digest,0,sizeof(digest));
                else
                {
                    rewind(fp);
                    if ( fread(&TD,1,sizeof(TD),fp) == sizeof(TD) )
                        KangarooTwelve((uint8_t *)&TD,sizeof(TD),digest,32);
                    else deleteflag = 1;
                }
                fclose(fp);
                if ( deleteflag == 0 )
                {
                    if ( memcmp(digest,T.transactionDigest,sizeof(digest)) == 0 )
                    {
                        if ( missed == 0 && tick > VALIDATED_TICK )
                        {
                            //printf("VALIDATED_TICK.%d\n",tick);
                            incr_VALIDATE_TICK(tick,T);
                        }
                    }
                    else
                    {
                        deleteflag = 1;
                        char hex1[65],hex2[65];
                        byteToHex(digest,hex1,32);
                        byteToHex(T.transactionDigest,hex2,32);
                        printf("transactiondigest mismatch tick.%d %s vs %s\n",T.tick,hex1,hex2);
                    }
                }
                if ( deleteflag != 0 )
                {
                    printf("validate deletes %s\n",fname);
                    deletefile(fname);
                }
            }
            else
            {
                if ( haveQ != 0 )
                {
                    memset(zeros,0,sizeof(zeros));
                    if ( memcmp(zeros,T.transactionDigest,sizeof(zeros)) == 0 )
                    {
                        if ( (fp= fopen(fname,"wb")) != 0 )
                        {
                            fwrite(zeros,1,sizeof(zeros),fp);
                            fclose(fp);
                        }
                    }
                }
                missed++;
            }
        }
        else
        {
            missed++;
            //printf("%d missing\n",tick);
        }
    }
    if ( (rand() % 100) == 0 )
        printf("VALIDATED_TICK.%d missed.%d of %d\n",VALIDATED_TICK,missed,n);
    return(VALIDATED_TICK);
}

int32_t cmptick(Tick *a,Tick *b)
{
    if ( a->epoch != b->epoch )
        return(-1);
    if ( a->tick != b->tick )
        return(-2);
    if ( a->millisecond != b->millisecond )
        return(-3);
    if ( a->second != b->second )
        return(-4);
    if ( a->minute != b->minute )
        return(-5);
    if ( a->hour != b->hour )
        return(-6);
    if ( a->day != b->day )
        return(-7);
    if ( a->month != b->month )
        return(-8);
    if ( a->year != b->year )
        return(-9);
    if ( a->prevResourceTestingDigest != b->prevResourceTestingDigest )
        return(-10);
    //if ( a->saltedResourceTestingDigest != b->saltedResourceTestingDigest )
    //    return(-11);
    if ( memcmp(a->prevSpectrumDigest,b->prevSpectrumDigest,sizeof(a->prevSpectrumDigest)) != 0 )
        return(-12);
    if ( memcmp(a->prevUniverseDigest,b->prevUniverseDigest,sizeof(a->prevUniverseDigest)) != 0 )
        return(-13);
    if ( memcmp(a->prevComputerDigest,b->prevComputerDigest,sizeof(a->prevComputerDigest)) != 0 )
        return(-14);
    //if ( memcmp(a->saltedSpectrumDigest,b->saltedSpectrumDigest,sizeof(a->saltedSpectrumDigest)) != 0 )
    //    return(-15);
    //if ( memcmp(a->saltedUniverseDigest,b->saltedUniverseDigest,sizeof(a->saltedUniverseDigest)) != 0 )
    //    return(-16);
    //if ( memcmp(a->saltedComputerDigest,b->saltedComputerDigest,sizeof(a->saltedComputerDigest)) != 0 )
    //    return(-17);
    if ( memcmp(a->transactionDigest,b->transactionDigest,sizeof(a->transactionDigest)) != 0 )
        return(-18);
    //if ( memcmp(a->expectedNextTickTransactionDigest,b->expectedNextTickTransactionDigest,sizeof(a->expectedNextTickTransactionDigest)) != 0 )
    //    return(-19);
    return(0);
}

int32_t quorumsigverify(Computors *computors,Tick *T)
{
    int computorIndex = T->computorIndex;
    uint8_t digest[32],computorOfThisTick[32];
    T->computorIndex ^= BROADCAST_TICK;
    KangarooTwelve((uint8_t *)T,sizeof(*T) - SIGNATURE_SIZE,digest,32);
    memcpy(computorOfThisTick,computors->publicKeys[computorIndex],32);
    if ( verify(computorOfThisTick,digest,T->signature) != 0 )
    {
        T->computorIndex ^= BROADCAST_TICK;
        return(1);
    }
    T->computorIndex ^= BROADCAST_TICK;
    return(0);
}

void process_quorumdata(int32_t peerid,char *ipaddr,Tick *qdata,Computors *computors)
{
    FILE *fp;
    char fname[512];
    int32_t i,offset,firsti,matches,count,errors,err;
    offset = (qdata->tick - INITIAL_TICK);
    if ( qdata->epoch == EPOCH && qdata->tick != 0 && offset >= 0 && offset < MAXTICKS && RAMQ[offset].validated == 0 )
    {
        if ( qdata->computorIndex >= 0 && qdata->computorIndex < 676 && RAMQ[offset].Quorum[qdata->computorIndex].tick != qdata->tick && quorumsigverify(computors,qdata) == 1 )
        {
            RAMQ[offset].Quorum[qdata->computorIndex] = *qdata;
            RAMQ[offset].count++;
            if ( RAMQ[offset].count >= 451 )
            {
                for (count=i=0; i<676; i++)
                {
                    //printf("%d ",RAMQ[offset].Quorum[i].tick);
                    if ( RAMQ[offset].Quorum[i].tick != 0 )
                        count++;
                }
                if ( count != RAMQ[offset].count )
                {
                    printf("set count %d -> %d\n",RAMQ[offset].count,count);
                    RAMQ[offset].count = count;
                }
                for (firsti=0; firsti<676; firsti++)
                    if ( RAMQ[offset].Quorum[firsti].epoch == EPOCH && RAMQ[offset].Quorum[firsti].tick == qdata->tick )
                        break;
                matches = 1;
                errors = 0;
                for (i=firsti+1; i<676; i++)
                {
                    if ( RAMQ[offset].Quorum[i].tick == 0 )
                       continue;
                    if ( (err= cmptick(&RAMQ[offset].Quorum[firsti],&RAMQ[offset].Quorum[i])) == 0 )
                        matches++;
                    else
                    {
                        errors++;
                        if ( err != -2 )
                            printf("E%d ",err);
                    }
                }
                if ( matches >= 451 )
                {
                    RAMQ[offset].validated = matches;
                    sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),qdata->tick);
                    if ( (fp= fopen(fname,"wb")) != 0 )
                    {
                        fwrite(RAMQ[offset].Quorum,1,sizeof(RAMQ[offset].Quorum),fp);
                        fclose(fp);
                    }
                }
                //printf("tick.%d peerid.%d matches %d, firsti.%d errors.%d count.%d\n",qdata->tick,peerid,matches,firsti,errors,count);
            }
        }
    }
}

int32_t updateticktx(int32_t tick,char *ipaddr,int32_t sock)
{
    RequestedTickTransactions TX;
    struct quheader H;
    uint8_t reqbuf[sizeof(H) + sizeof(TX)];
    memset(&TX,0,sizeof(TX));
    memset(reqbuf,0,sizeof(reqbuf));
    H = quheaderset(REQUEST_TICK_TRANSACTIONS,sizeof(H) + sizeof(TX));
    memcpy(reqbuf,&H,sizeof(H));
    TX.tick = tick;
    memcpy(&reqbuf[sizeof(H)],&TX,sizeof(TX));
    //printf(">>>>>>>>>>> %s updateticktx %d\n",ipaddr,tick);
    return(socksend(ipaddr,sock,reqbuf,sizeof(H) + sizeof(TX)));
}

int32_t updatetick(int32_t *nump,int32_t tick,char *ipaddr,int32_t sock)
{
    FILE *fp;
    char fname[512];
    struct quheader H;
    RequestedQuorumTick R;
    int32_t offset;
    uint8_t reqbuf[sizeof(H) + sizeof(R) ];
    sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
    if ( (fp= fopen(fname,"rb")) == 0 )
    {
        sock = updateticktx(tick,ipaddr,sock);
        H = quheaderset(REQUEST_TICK_DATA,sizeof(H) + sizeof(tick));
        memcpy(reqbuf,&H,sizeof(H));
        memcpy(&reqbuf[sizeof(H)],&tick,sizeof(tick));
        sock = socksend(ipaddr,sock,reqbuf,sizeof(H) + sizeof(tick));
        (*nump)++;
    } else fclose(fp);
    sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),tick);
    offset = (tick - INITIAL_TICK);
    if ( offset >= 0 && offset < MAXTICKS )//&& RAMQ[offset].pending < LATEST_TICK-1 )
    {
        if ( (fp= fopen(fname,"rb")) == 0 )
        {
            memset(&R,0,sizeof(R));
            memset(reqbuf,0,sizeof(reqbuf));
            H = quheaderset(REQUEST_QUORUMTICK,sizeof(H) + sizeof(R));
            memcpy(reqbuf,&H,sizeof(H));
            R.tick = tick;
            memcpy(&reqbuf[sizeof(H)],&R,sizeof(R));
            sock = socksend(ipaddr,sock,reqbuf,sizeof(H) + sizeof(R));
            RAMQ[offset].pending = LATEST_TICK;
            (*nump) += 2;
        } else fclose(fp);
    }
    return(sock);
}

void addnewpeer(char *ipaddr)
{
    int32_t i,flag = 0;
    pthread_mutex_lock(&addpeer_mutex);
    for (i=0; i<(sizeof(Peers)/sizeof(*Peers)); i++)
        if ( strcmp(ipaddr,Peers[i]) == 0 )
        {
            flag = 1;
            break;
        }
    for (i=0; i<Numnewpeers; i++)
        if ( strcmp(ipaddr,Newpeers[i]) == 0 )
        {
            flag = 1;
            break;
        }
    if ( flag == 0 )
    {
        strcpy(Newpeers[i],ipaddr);
        //printf("Newpeer.%d %s\n",Numnewpeers,ipaddr);
        Numnewpeers++;
    }
    pthread_mutex_unlock(&addpeer_mutex);
}

void process_publicpeers(int32_t peerid,char *ipaddr,ExchangePublicPeers *peers)
{
    int32_t i,j;
    char peeraddr[64];
    for (i=0; i<4; i++)
    {
        peeraddr[0] = 0;
        for (j=0; j<4; j++)
            sprintf(peeraddr + strlen(peeraddr),"%d%c",peers->peers[i][j],j<3?'.':0);
        addnewpeer(peeraddr);
    }
}

void process_tickinfo(int32_t peerid,char *ipaddr,CurrentTickInfo *I)
{
    Peertickinfo[peerid].info = *I;
    if ( I->tick > LATEST_TICK )
    {
        LATEST_TICK = I->tick;
        if ( I->initialTick > INITIAL_TICK )
        {
            if ( INITIAL_TICK != 0 )
            {
                memset(RAMQ,0,sizeof(*RAMQ) * MAXTICKS);
                VALIDATED_TICK = 0;
                HAVE_TXTICK = 0;
            }
            INITIAL_TICK = I->initialTick;
        }
        if ( I->epoch > EPOCH )
            EPOCH = I->epoch;
        printf("%s epoch.%d tick.%d LATEST.%d lag.%d, INITIAL_TICK.%d\n",ipaddr,EPOCH,I->tick,LATEST_TICK,LATEST_TICK - I->tick,INITIAL_TICK);
    }
}

void process_entity(int32_t peerid,char *ipaddr,RespondedEntity *E)
{
    struct addrhash *ap;
    if ( (ap= Addresshash(E->entity.publicKey,0)) != 0 )
    {
        char addr[64];
        pubkey2addr(E->entity.publicKey,addr);
        //for (int j=0; j<32; j++)
        //    printf("%02x",E->entity.publicKey[j]);
        printf(" %s got entity %s %s tick.%d vs %d LATEST.%d VALIDATED.%d HAVETX.%d\n",ipaddr,addr,amountstr(E->entity.incomingAmount - E->entity.outgoingAmount),E->tick,ap->tick,LATEST_TICK,VALIDATED_TICK,HAVE_TXTICK);
        if ( E->tick > ap->tick )
        {
            memcpy(&ap->entity,&E->entity,sizeof(ap->entity));
            ap->tick = E->tick;
            merkleRoot(SPECTRUM_DEPTH,E->spectrumIndex,(uint8_t *)&ap->entity,sizeof(ap->entity),&E->siblings[0][0],ap->merkleroot);
        }
        // do merkle validation
    } else printf("unexpected entity data without address?\n");
}

int32_t process_transaction(int32_t *savedtxp,FILE *fp,int32_t peerid,char *ipaddr,Transaction *tx,int32_t txlen)
{
    static uint8_t txdata[MAXPEERS+1][4096],sigs[MAXPEERS+1][64]; // needs to be aligned or crashes in verify
    uint8_t digest[32],txid[32];
    char addr[64],txidstr[64];
    int32_t v = -1;
    if ( txlen < sizeof(txdata[peerid]) )
    {
        memcpy(txdata[peerid],tx,txlen);
        KangarooTwelve(txdata[peerid],txlen,txid,32);
        getTxHashFromDigest(txid,txidstr);
        txidstr[60] = 0;
        KangarooTwelve(txdata[peerid],txlen-64,digest,32);
        memcpy(sigs[peerid],&txdata[peerid][txlen-64],64);
        v = verify(txdata[peerid],digest,sigs[peerid]);
        pubkey2addr(tx->sourcePublicKey,addr);
        if ( v > 0 && fp != 0 )
        {
            fwrite(&txlen,1,sizeof(txlen),fp);
            fwrite(txid,1,sizeof(txid),fp);
            fwrite(tx,1,txlen,fp);
            qubictxhash(txid,(uint8_t *)tx,txlen);
            (*savedtxp)++;
        }
        //printf("process tx from tick.%d %s %s v.%d txlen.%d %p\n",tx->tick,txidstr,addr,v,txlen,qubictxhash(txid,(uint8_t *)tx,txlen));
    }
    return(v);
}

int32_t validate_computors(Computors *computors)
{
    uint8_t digest[32],arbPubkey[32];
    addr2pubkey(ARBITRATOR,arbPubkey);
    KangarooTwelve((uint8_t *)computors,sizeof(*computors) - SIGNATURE_SIZE,digest,32);
    return(verify(arbPubkey,digest,computors->signature));
}

int32_t has_computors(Computors *computors,char *ipaddr)
{
    FILE *fp;
    char fname[512];
    sprintf(fname,"epochs%c%d%ccomputors%c%s",dir_delim(),EPOCH,dir_delim(),dir_delim(),ipaddr);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        fread(computors,1,sizeof(*computors),fp);
        fclose(fp);
        return(validate_computors(computors));
    }
    return(-1);
}

void process_computors(int32_t peerid,char *ipaddr,Computors *computors)
{
    int32_t v;
    FILE *fp;
    char fname[512];
    v = validate_computors(computors);
    if ( v != 0 )
    {
        sprintf(fname,"epochs%c%d%ccomputors%c%s",dir_delim(),EPOCH,dir_delim(),dir_delim(),ipaddr);
        if ( (fp= fopen(fname,"rb")) == 0 )
        {
            if ( (fp= fopen(fname,"wb")) != 0 )
            {
                fwrite(computors,1,sizeof(*computors),fp);
                fclose(fp);
            }
        } else fclose(fp);
    }
    printf("peerid.%d computors.%d v.%d\n",peerid,EPOCH,v);
}

void process_tickdata(int32_t peerid,char *ipaddr,TickData *tickdata,Computors *computors)
{
    FILE *fp;
    char fname[512];
    uint8_t sigcheck[32],sig[64],pubkey[32];
    int32_t computorIndex = tickdata->computorIndex;
    tickdata->computorIndex ^= BROADCAST_FUTURE_TICK_DATA;
    KangarooTwelve((uint8_t *)tickdata,sizeof(*tickdata) - SIGNATURE_SIZE,sigcheck,32);
    tickdata->computorIndex ^= BROADCAST_FUTURE_TICK_DATA;
    memcpy(sig,tickdata->signature,sizeof(tickdata->signature));
    memcpy(pubkey,computors->publicKeys[computorIndex],sizeof(pubkey));
    if ( verify(pubkey,sigcheck,sig) != 0 )
    {
        //printf("process tickdata %d GOOD SIG! computor.%d\n",tickdata->tick,computorIndex);
        sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tickdata->tick);
        if ( (fp= fopen(fname,"wb")) != 0 )
        {
            fwrite(tickdata,1,sizeof(*tickdata),fp);
            fclose(fp);
        }
    }
    //else printf("process tickdata %d sig error computor.%d\n",tickdata->tick,computorIndex);
}

void process_issued(int32_t peerid,char *ipaddr,RespondIssuedAssets *issued)
{
    
}

void process_owned(int32_t peerid,char *ipaddr,RespondOwnedAssets *owned)
{
    
}

void process_possessed(int32_t peerid,char *ipaddr,RespondPossessedAssets *possessed)
{
    
}

int32_t process_response(int32_t *savedtxp,FILE *fp,Computors *computors,int32_t peerid,char *ipaddr,struct quheader *H,void *data,int32_t datasize)
{
    switch ( H->_type )
    {
        case EXCHANGE_PUBLIC_PEERS:         if ( datasize != sizeof(ExchangePublicPeers) ) return(-1);
            process_publicpeers(peerid,ipaddr,(ExchangePublicPeers *)data);
            break;
        case BROADCAST_COMPUTORS:           if ( datasize != sizeof(Computors) ) return(-1);
            process_computors(peerid,ipaddr,(Computors *)data);
            break;
        case BROADCAST_TICK:                if ( datasize != sizeof(Tick) ) return(-1);
            process_quorumdata(peerid,ipaddr,(Tick *)data,computors);
            break;
        case BROADCAST_FUTURE_TICK_DATA:    if ( datasize > sizeof(TickData) ) return(-1);
            process_tickdata(peerid,ipaddr,(TickData *)data,computors);
            break;
        case BROADCAST_TRANSACTION:         if ( datasize < sizeof(Transaction) ) return(-1);
            process_transaction(savedtxp,fp,peerid,ipaddr,(Transaction *)data,datasize);
            break;
        case RESPOND_CURRENT_TICK_INFO:     if ( datasize != sizeof(CurrentTickInfo) ) return(-1);
            process_tickinfo(peerid,ipaddr,(CurrentTickInfo *)data);
            break;
        case RESPOND_ENTITY:                if ( datasize != sizeof(RespondedEntity) ) return(-1);
            process_entity(peerid,ipaddr,(RespondedEntity *)data);
            break;
        case RESPOND_ISSUED_ASSETS:         if ( datasize != sizeof(RespondIssuedAssets) ) return(-1);
            process_issued(peerid,ipaddr,(RespondIssuedAssets *)data);
            break;
        case RESPOND_OWNED_ASSETS:          if ( datasize != sizeof(RespondOwnedAssets) ) return(-1);
            process_owned(peerid,ipaddr,(RespondOwnedAssets *)data);
            break;
        case RESPOND_POSSESSED_ASSETS:      if ( datasize != sizeof(RespondPossessedAssets) ) return(-1);
            process_possessed(peerid,ipaddr,(RespondPossessedAssets *)data);
            break;
        case REQUEST_SYSTEM_INFO: // we are not server
            break;
        //case PROCESS_SPECIAL_COMMAND:       if ( datasize != sizeof() ) return(-1);
        default: printf("%s unknown type.%d sz.%d\n",ipaddr,H->_type,datasize);
            break;
    }
    //printf("peerid.%d got %d cmd.%d from %s\n",peerid,datasize,H->_type,ipaddr);
    return(0);
}

int32_t updateticks(char *ipaddr,int32_t sock)
{
    int32_t i,tick,n,numsent,firsttick,numticks,offset,ind;
    if ( Numpeers == 0 )
        return(sock);
    ind = (rand() % Numpeers);
    firsttick = (VALIDATED_TICK > INITIAL_TICK) ? VALIDATED_TICK : INITIAL_TICK-1;
    numticks = (LATEST_TICK - firsttick);
    n = 10 * numticks / Numpeers;
    if ( n < 676 )
        n = 676;
    numsent = 0;
    if ( VALIDATED_TICK == 0 )
        sock = updatetick(&numsent,INITIAL_TICK,ipaddr,sock);
    else sock = updatetick(&numsent,VALIDATED_TICK+1,ipaddr,sock);
    if ( HAVE_TXTICK == 0 )
        sock = updateticktx(INITIAL_TICK,ipaddr,sock);
    else
        sock = updateticktx(HAVE_TXTICK+1,ipaddr,sock);
    if ( LATEST_TICK - VALIDATED_TICK < 30 )
    {
        for (tick=VALIDATED_TICK+2; tick<=LATEST_TICK; tick++)
            sock = updatetick(&numsent,tick,ipaddr,sock);
    }
    else
    {
        for (i=0; i<n; i++)
        {
            tick = firsttick + i*Numpeers + ind + 1;
            if ( tick > LATEST_TICK+5 )
                break;
            sock = updatetick(&numsent,tick,ipaddr,sock);
            if ( numsent > QUORUM_FETCHWT )
                break;
        }
    }
    if ( numsent < TX_FETCHWT )
    {
        firsttick = (HAVE_TXTICK > INITIAL_TICK) ? HAVE_TXTICK : INITIAL_TICK-1;
        for (i=0; i<n && numsent<TX_FETCHWT; i++)
        {
            tick = firsttick + i*Numpeers + ind + 1;
            if ( tick > LATEST_TICK+5 )
                break;
            offset = (tick - INITIAL_TICK);
            if ( offset >= 0 && offset < MAXTICKS && RAMQ[offset].needtx != 0 )
            {
                sock = updateticktx(tick,ipaddr,sock);
                numsent++;
            }
        }
    }
    return(sock);
}

void peertxfname(char *fname,char *ipaddr)
{
    sprintf(fname,"epochs%c%d%ctx%c%s",dir_delim(),EPOCH,dir_delim(),dir_delim(),ipaddr);
}

void *peerthread(void *_ipaddr)
{
    char *ipaddr = _ipaddr;
    struct EntityRequest E;
    Computors computors;
    FILE *fp;
    char fname[512],addr[64];
    int32_t peerid,sock=-1,i,hashi,savedtx,lasthashi,ptr,sz,recvbyte,prevutime,prevtick,iter,bufsize,hascomputors;
    struct quheader H;
    struct addrhash *ap;
    uint8_t *buf;
    signal(SIGPIPE, SIG_IGN);
    bufsize = 4096 * 1024;
    buf = calloc(1,bufsize);
    peerid = prevutime = prevtick = iter = hascomputors = 0;
    while ( iter++ < 10 )
    {
        if ( (sock= myconnect(ipaddr,DEFAULT_NODE_PORT)) < 0 )
        {
            //printf("iter.%d peerthread error connecting to %s\n",iter,ipaddr);
        }
        else break;
    }
    if ( iter >= 10 )
        return(0);
    pthread_mutex_lock(&addpeer_mutex);
    if ( Numpeers < MAXPEERS-1 ) // nonzero peerid wastes a slot but no big loss
    {
        Numpeers++;
        peerid = Numpeers;
    }
    else
    {
        printf("maxpeers %d reached\n",Numpeers);
        pthread_mutex_unlock(&addpeer_mutex);
        return(0);
    }
    pthread_mutex_unlock(&addpeer_mutex);
    strcpy(Peertickinfo[peerid].ipaddr,ipaddr);
    lasthashi = (rand() % MAXADDRESSES);
    peertxfname(fname,ipaddr);
    if ( (fp= fopen(fname,"rb+")) != 0 )
        fseek(fp,0,SEEK_END);
    else fp = fopen(fname,"wb");
    printf("connected.%d peerthread %s\n",Numpeers,ipaddr);
    H = quheaderset(REQUEST_CURRENT_TICK_INFO,sizeof(H));
    sock = socksend(ipaddr,sock,(uint8_t *)&H,sizeof(H));
    while ( 1 )
    {
        if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
            break;
        if ( hascomputors == 0 && has_computors(&computors,ipaddr) <= 0 )
        {
            H = quheaderset(REQUEST_COMPUTORS,sizeof(H));
            sock = socksend(ipaddr,sock,(uint8_t *)&H,sizeof(H));
        } else hascomputors = 1;
        savedtx = 0;
        while ( (recvbyte= receiveall(sock,buf,bufsize)) > 0 )
        {
            ptr = 0;
            sz = 1;
            while ( ptr < recvbyte && sz != 0 && sz < recvbyte )
            {
                memcpy(&H,&buf[ptr],sizeof(H));
                sz = ((H._size[2] << 16) + (H._size[1] << 8) + H._size[0]);
                //for (int j=0; j<sz; j++)
                //    printf("%02x",buf[ptr+j]);
                //printf(" %s received %d H.(%d %d bytes)\n",ipaddr,recvbyte,H._type,sz);
                if ( sz < 1 || sz > recvbyte-ptr )
                {
                    //printf("illegal sz.%d vs recv.%d ptr.%d type.%d\n",sz,recvbyte,ptr,H._type);
                    break;
                }
                if ( H._type == 35 && sz == sizeof(H) )
                {
                }
                else
                {
                    if ( process_response(&savedtx,fp,&computors,peerid,ipaddr,&H,&buf[ptr + sizeof(H)],sz - sizeof(H)) < 0 )
                    {
                        //printf("peerid.%d Error processing H.type %d size.%ld\n",peerid,H._type,sz - sizeof(H));
                    }
                }
                ptr += sz;
            }
        }
        if ( fp != 0 && savedtx != 0 )
            fflush(fp);
        if ( Peertickinfo[peerid].packetlen != 0 )
        {
            printf("%s sends packet[%d]\n",ipaddr,Peertickinfo[peerid].packetlen);
            sock = socksend(ipaddr,sock,Peertickinfo[peerid].packet,Peertickinfo[peerid].packetlen);
            Peertickinfo[peerid].packetlen = 0;
        }
        if ( EXITFLAG == 0 && LATEST_UTIME > prevutime )
        {
            if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
                break;
            prevutime = LATEST_UTIME;
            H = quheaderset(REQUEST_CURRENT_TICK_INFO,sizeof(H));
            sock = socksend(ipaddr,sock,(uint8_t *)&H,sizeof(H));
            sock = updateticks(ipaddr,sock);
            if ( Peertickinfo[peerid].info.tick < LATEST_TICK - 100 )
            {
                //printf("peerid.%d latest.%d lag.%d, skip entity request\n",peerid,Peertickinfo[peerid].tick,LATEST_TICK-Peertickinfo[peerid].tick);
            }
            else
            {
                //printf("peerid.%d lasthashi.%d\n",peerid,lasthashi);
                for (hashi=i=0; i<ADDRSCAN_DEPTH; i++)
                {
                    hashi = (lasthashi + i) % MAXADDRESSES;
                    ap = &Addresses[hashi];
                    if ( ap->tick == 0 )//|| ap->tick >= Peertickinfo[peerid].info.tick )
                        continue;
                    pubkey2addr(ap->entity.publicKey,addr);
                    //printf("peerid.%d >>>>>>>>>>>>> found %s at %d\n",peerid,addr,hashi);
                    memset(&E,0,sizeof(E));
                    E.H = quheaderset(REQUEST_ENTITY,sizeof(E));
                    memcpy(E.pubkey,ap->entity.publicKey,sizeof(E.pubkey));
                    sock = socksend(ipaddr,sock,(uint8_t *)&E,sizeof(E));
                }
                lasthashi = hashi;
                //printf("peerid.%d lasthashi.%d\n",peerid,lasthashi);
            }
        }
        if ( LATEST_TICK > prevtick )
        {
            prevtick = LATEST_TICK;
        } else usleep(1000);
    }
    if ( sock >= 0 )
        close(sock);
    if ( fp != 0 )
        fclose(fp);
    printf("%s exits thread\n",ipaddr);
    return(0);
}

int32_t peerbroadcast(uint8_t *packet,int32_t packetlen)
{
    int32_t peerid,n = 0;
    for (peerid=0; peerid<Numpeers; peerid++)
    {
        if ( Peertickinfo[peerid].info.tick > LATEST_TICK-100 && Peertickinfo[peerid].packetlen == 0 )
        {
            memcpy(Peertickinfo[peerid].packet,packet,packetlen);
            Peertickinfo[peerid].packetlen = packetlen;
            n++;
        }
    }
    return(n);
}

void *findpeers(void *args)
{
    pthread_t peer_threads[sizeof(Peers)/sizeof(*Peers)];
    pthread_t newpeer_threads[sizeof(Newpeers)/sizeof(*Newpeers)];
    int32_t i,num;
    for (i=0; i<(sizeof(Peers)/sizeof(*Peers)); i++)
        pthread_create(&peer_threads[i],NULL,&peerthread,(void *)Peers[i]);
    while ( Numnewpeers < (sizeof(Newpeers)/sizeof(*Newpeers)) )
    {
        num = Numnewpeers;
        while ( Numnewpeers == num )
            sleep(1);
        for (i=num; i<Numnewpeers && i<(sizeof(Newpeers)/sizeof(*Newpeers)); i++)
            pthread_create(&newpeer_threads[i],NULL,&peerthread,(void *)Newpeers[i]);
        if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
            break;
    }
    printf("find peers thread finished\n");
    return(0);
}

int32_t loadqtx(long *fposp,FILE *fp)
{
    int32_t txlen,retval = -1;
    char txidstr[64];
    struct qubictx *qtx;
    uint8_t txid[32],cmptxid[32],txdata[MAX_INPUT_SIZE*2];
    if ( fread(&txlen,1,sizeof(txlen),fp) != sizeof(txlen) )
    {
        //printf("error reading txlen at pos %ld\n",*fposp);
    }
    else if ( txlen < sizeof(txdata) )
    {
        if ( fread(txid,1,sizeof(txid),fp) != sizeof(txid) )
        {
            //printf("error reading txid at pos %ld\n",*fposp);
        }
        else if ( fread(txdata,1,txlen,fp) != txlen )
        {
            //printf("error reading txdata at pos %ld\n",*fposp);
        }
        else
        {
            KangarooTwelve(txdata,txlen,cmptxid,32);
            getTxHashFromDigest(txid,txidstr);
            txidstr[60] = 0;
            if ( memcmp(txid,cmptxid,sizeof(txid)) == 0 )
            {
                *fposp = ftell(fp);
                HASH_FIND(hh,TXIDS,txid,sizeof(qtx->txid),qtx);
                if ( qtx == 0 )
                {
                    _qubictxadd(txid,txdata,txlen);
                    retval = 1;
                } else retval = 0;
            }
            //else printf("error comparing %s at pos %ld\n",txidstr,*fposp);
        }
    }
    //else
    //    printf("illegal txlen.%d pos %ld\n",txlen,*fposp);
    return(retval);
}

int32_t havealltx(int32_t tick) // negative means error, 0 means file exists, 1 means file created
{
    char fname[512],txidstr[64];
    uint8_t zero[32];
    FILE *fp;
    TickData TD;
    struct qubictx *qtx,*ptrs[NUMBER_OF_TRANSACTIONS_PER_TICK];
    int32_t i,sz,numtx,haveQ;
    Tick T;
    sprintf(fname,"epochs%c%d%c%d.T",dir_delim(),EPOCH,dir_delim(),tick);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        fclose(fp);
        return(0);
    }
    sprintf(fname,"epochs%c%d%c%d.Q",dir_delim(),EPOCH,dir_delim(),tick);
    haveQ = 0;
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        while ( fread(&T,1,sizeof(T),fp) == sizeof(T) )
        {
            if ( T.tick == tick )
            {
                haveQ = 1;
                break;
            }
        }
        fclose(fp);
        if ( haveQ != 0 )
        {
            memset(zero,0,sizeof(zero));
            if ( memcmp(T.transactionDigest,zero,32) == 0 )
            {
                //printf("empty tick.%d\n",tick);
                sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
                if ( (fp= fopen(fname,"wb")) != 0 )
                {
                    fwrite(zero,1,sizeof(zero),fp);
                    fclose(fp);
                }
                return(0);
            }
        }
    }
    sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        if ( (sz= (int32_t)fread(&TD,1,sizeof(TD),fp)) == 32 ) // empty tick
        {
            fclose(fp);
            return(0);
        }
        fclose(fp);
        if ( sz == sizeof(TD) )
        {
            memset(ptrs,0,sizeof(ptrs));
            memset(zero,0,sizeof(zero));
            for (numtx=i=0; i<NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
            {
                if ( memcmp(zero,TD.transactionDigests[i],sizeof(zero)) != 0 )
                {
                    qtx = qubictxhash(TD.transactionDigests[i],0,0);
                    if ( qtx != 0 )
                        ptrs[numtx++] = qtx;
                    else
                    {
                        getTxHashFromDigest(TD.transactionDigests[i],txidstr);
                        txidstr[60] = 0;
                        //printf("tick.%d missing txi.%d after numtx.%d %s\n",tick,i,numtx,txidstr);
                        return(-2);
                    }
                }
            }
            if ( numtx == 0 )
                return(0);
            sprintf(fname,"epochs%c%d%c%d.T",dir_delim(),EPOCH,dir_delim(),tick);
            if ( (fp= fopen(fname,"wb")) == 0 )
                return(-3);
            for (i=0; i<numtx; i++)
            {
                if ( (qtx= ptrs[i]) == 0 )
                    break;
                if ( fwrite(&qtx->txlen,1,sizeof(qtx->txlen),fp) != sizeof(qtx->txlen) )
                    break;
                if ( fwrite(qtx->txid,1,sizeof(qtx->txid),fp) != sizeof(qtx->txid) )
                    break;
                if ( fwrite(qtx->txdata,1,qtx->txlen,fp) != qtx->txlen )
                    break;
            }
            fclose(fp);
            if ( i != numtx )
            {
                deletefile(fname);
                return(-4);
            }
        }
        else
            return(-5);
    }
    return(-1);
}

int32_t init_txids(void)
{
    char fname[512],line[512],cmd[1024],*ptr;
    FILE *fp,*fp2;
    long fpos;
    int32_t retval,numfiles = 0,numadded = 0;
    sprintf(cmd,"ls -l epochs%c%d%ctx > txfiles",dir_delim(),EPOCH,dir_delim());
    //sprintf(cmd,"ls -w 16 epochs%c%d%ctx > txfiles",dir_delim(),EPOCH,dir_delim());
    system(cmd);
    printf("%s\n",cmd);
    if ( (fp= fopen("txfiles","r")) != 0 )
    {
        while ( fgets(line,sizeof(line),fp) != 0 )
        {
            ptr = &line[strlen(line)-1];
            *ptr = 0;
            while ( ptr > line && ptr[-1] != ' ' )
                ptr--;
            printf("(%s) [%s]\n",line,ptr);
            sprintf(fname,"epochs%c%d%ctx%c%s",dir_delim(),EPOCH,dir_delim(),dir_delim(),ptr);
            if ( (fp2= fopen(fname,"rb")) != 0 )
            {
                numfiles++;
                fpos = 0;
                while ( 1 )
                {
                    if ( (retval= loadqtx(&fpos,fp2)) < 0 )
                        break;
                    else numadded += (retval != 0);
                }
                fclose(fp2);
            }
        }
        fclose(fp);
    }
    printf("added %d txids from %d files\n",numadded,numfiles);
    return(numadded);
}

void update_txids(void) // single threaded
{
    int32_t offset,retval,tick,missed = 0;
    /*static FILE *fps[MAXPEERS];
    static long fpos[MAXPEERS];
     char fname[512];
     for (peerid=1; peerid<MAXPEERS; peerid++)
    {
        if ( Peertickinfo[peerid].ipaddr[0] != 0 && fps[peerid] == 0 )
        {
            peertxfname(fname,Peertickinfo[peerid].ipaddr);
            fps[peerid] = fopen(fname,"rb");
        }
        if ( fps[peerid] != 0 )
        {
            fseek(fps[peerid],0,SEEK_END);
            if ( ftell(fps[peerid]) >= fpos[peerid]+sizeof(int32_t)+32+sizeof(Transaction) )
            {
                while ( 1 )
                {
                    fseek(fps[peerid],fpos[peerid],SEEK_SET);
                    if ( loadqtx(&fpos[peerid],fps[peerid]) < 0 )
                        break;
                }
            }
        }
    }*/
    if ( HAVE_TXTICK < VALIDATED_TICK )
    {
        if ( HAVE_TXTICK == 0 )
            tick = INITIAL_TICK;
        else tick = HAVE_TXTICK+1;
        for (; tick<=VALIDATED_TICK; tick++)
        {
            if ( (retval= havealltx(tick)) >= 0 ) // negative means error, 0 means file exists, 1 means file created
            {
                if ( missed == 0 )
                {
                    PROGRESSTIME = LATEST_UTIME;
                    EXITFLAG = 0;
                    HAVE_TXTICK = tick;
                }
                //else printf("havealltx gap.%d\n",tick - HAVE_TXTICK);
            }
            else
            {
                offset = (tick - INITIAL_TICK);
                if ( offset >= 0 && offset < MAXTICKS )
                    RAMQ[offset].needtx = 1;
                missed++;
                //printf("retval.%d for tick.%d\n",retval,tick);
            }
        }
    }
}

void entityjson(char *str,char *addr,struct Entity E,int32_t tick,uint8_t merkle[32],int64_t sent,int64_t recv)
{
    char merklestr[65];
    byteToHex(merkle,merklestr,32);
    sprintf(str,"{\"address\":\"%s\",\"balance\":\"%s\",\"sent\":\"%s\",\"received\":\"%s\",\"tick\":%d,\"spectrum\":\"%s\",\"numin\":%d,\"totalincoming\":\"%s\",\"latestin\":%d,\"numout\":%d,\"totaloutgoing\":\"%s\",\"latestout\":%d,\"command\":\"EntityInfo\",\"wasm\":1}",addr,amountstr(E.incomingAmount - E.outgoingAmount),amountstr2(sent),amountstr3(recv),tick,merklestr,E.numberOfIncomingTransfers,amountstr4(E.incomingAmount),E.latestIncomingTransferTick,E.numberOfOutgoingTransfers,amountstr5(E.outgoingAmount),E.latestOutgoingTransferTick);
}

void newepoch(void)
{
    char dirname[512];
    sprintf(dirname,"epochs%c%d",dir_delim(),EPOCH);
    makedir(dirname);
    sprintf(dirname,"epochs%c%d%ccomputors",dir_delim(),EPOCH,dir_delim());
    makedir(dirname);
    sprintf(dirname,"epochs%c%d%ctx",dir_delim(),EPOCH,dir_delim());
    makedir(dirname);
}

void *dataloop(void *_ignore)
{
    uint32_t prevutime = 0;
    printf("dataloop STARTED\n");
    while ( 1 )
    {
        if ( LATEST_UTIME > prevutime )
        {
            prevutime = LATEST_UTIME;
            update_txids();
        }
        usleep(10000);
        if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
            break;
    }
    printf("dataloop exits\n");
    return(0);
}

void *validateloop(void *_ignore)
{
    int32_t latest = 0;
    printf("validateloop STARTED\n");
    while ( 1 )
    {
        if ( LATEST_TICK > latest )
        {
            latest = LATEST_TICK;
            update_validated();
        }
        sleep(1);
        if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
            break;
    }
    printf("validateloop exits\n");
    return(0);
}

int32_t ticktxlist(int32_t tick)
{
    FILE *fp;
    char fname[512],txidstr[64];
    TickData TD;
    int32_t i,n=0;
    uint8_t zero[32];
    sprintf(fname,"epochs%c%d%c%d",dir_delim(),EPOCH,dir_delim(),tick);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        fread(&TD,1,sizeof(TD),fp);
        fclose(fp);
        memset(zero,0,sizeof(zero));
        for (i=0; i<NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
        {
            if ( memcmp(TD.transactionDigests[i],zero,sizeof(zero)) == 0 )
                break;
            getTxHashFromDigest(TD.transactionDigests[i],txidstr);
            txidstr[60] = 0;
            printf("%s ",txidstr);
            n++;
        }
    }
    printf("tick %d has %d tx\n",tick,n);
    return(n);
}

int32_t brequest_process(struct brequest *rp)
{
    char fname[512],addr[64];
    struct addrhash AH,prevAH;
    FILE *fp;
    int32_t tick,retval = 0,prevtick,n = 0;
   // if ( VALIDATED_TICK < rp->tick || HAVE_TXTICK < rp->tick )
     //   return(0);
    memset(&AH,0,sizeof(AH));
    memset(&prevAH,0,sizeof(prevAH));
    pubkey2addr(rp->pubkey,addr);
    sprintf(fname,"addrs%c%s",dir_delim(),addr);
    if ( (fp= fopen(fname,"rb")) != 0 )
    {
        prevtick = 0;
        while ( fread(&AH,1,sizeof(AH.entity)+40,fp) == sizeof(AH.entity)+40 )
        {
            if ( AH.tick <= 1 || memcmp(AH.entity.publicKey,rp->pubkey,sizeof(rp->pubkey)) != 0 )
                continue;
            if ( AH.tick >= prevtick )
            {
                // check merkleroot
                if ( n++ != 0 )
                {
                    if ( (AH.entity.incomingAmount != prevAH.entity.incomingAmount || AH.entity.outgoingAmount != prevAH.entity.outgoingAmount) )
                    {
                        printf("n.%d t.%d: recv %s sent %s fpos.%ld/%ld\n",n,AH.tick,amountstr(AH.entity.incomingAmount),amountstr3(AH.entity.outgoingAmount),ftell(fp),rp->fpos);
                        for (tick=prevAH.tick; tick<=AH.tick; tick++)
                            ticktxlist(tick);
                        printf("BALANCE CHANGE EVENT t.%d to t.%d sent.%s recv.%s\n",prevAH.tick,AH.tick,amountstr(AH.entity.outgoingAmount != prevAH.entity.outgoingAmount),amountstr2(AH.entity.incomingAmount != prevAH.entity.incomingAmount));
                        //rp->prevtick = prevAH.tick;
                        //printf("n.%d t.%d -> t.%d: recv %s/%s sent %s/%s fpos.%ld/%ld\n",n,prevAH.tick,AH.tick,amountstr(AH.entity.incomingAmount - prevAH.entity.incomingAmount),amountstr2(rp->recv),amountstr3(AH.entity.outgoingAmount - prevAH.entity.outgoingAmount),amountstr4(rp->sent),ftell(fp),rp->fpos);
                    }
                    prevAH = AH;
                } else prevAH = AH;
                prevtick = AH.tick;
            }
            else
                printf("pubkey mismatch %s at fpos %ld\n",addr,ftell(fp));
        }
        fclose(fp);
    }
    return(retval);
}

struct brequest *brequest_poll(void)
{
    struct brequest *rp,*tmp;
    pthread_mutex_lock(&balancechange_mutex);
    DL_FOREACH_SAFE(BALANCEQ,rp,tmp)
    {
        printf("event tick.%d VALIDATED.%d\n",rp->tick,VALIDATED_TICK);
        brequest_process(rp);
        //if ( VALIDATED_TICK >= rp->tick && brequest_process(rp) == 1 )
            DL_DELETE(BALANCEQ,rp);
    }
    pthread_mutex_unlock(&balancechange_mutex);
    return(rp);
}

struct brequest *balancechanged(char *addr,struct addrhash *ap,int64_t sent,int64_t recv,long fpos)
{
    FILE *fp;
    char fname[512];
    struct brequest *rp;
    sprintf(fname,"addrs%c%s.events",dir_delim(),addr);
    rp = (struct brequest *)calloc(1,sizeof(*rp));
    memcpy(rp->pubkey,ap->entity.publicKey,32);
    rp->sent = sent;
    rp->recv = recv;
    rp->tick = ap->tick;
    rp->fpos = fpos;
    if ( (fp= fopen(fname,"rb+")) != 0 )
        fseek(fp,0,SEEK_END);
    else fp = fopen(fname,"wb");
    if ( fp != 0 )
    {
        fwrite(rp,1,sizeof(*rp),fp);
        fclose(fp);
    }
    pthread_mutex_lock(&balancechange_mutex);
    DL_APPEND(BALANCEQ,rp);
    pthread_mutex_unlock(&balancechange_mutex);
    return(rp);
}

void txjson(char *txidstr,char *jsonstr)
{
    struct qubictx *qtx;
    Transaction tx;
    uint8_t txid[32];
    char srcstr[64],deststr[64],extrastr[1026];
    txid2digest(txidstr,txid);
    qtx = qubictxhash(txid,0,0);
    extrastr[0] = 0;
    if ( qtx != 0 )
    {
        memcpy(&tx,qtx->txdata,sizeof(tx));
        pubkey2addr(tx.sourcePublicKey,srcstr);
        pubkey2addr(tx.destinationPublicKey,deststr);
        if ( tx.inputSize <= 512 )
            byteToHex(&qtx->txdata[sizeof(tx)],extrastr,tx.inputSize);
    }
    else
    {
        memset(&tx,0,sizeof(tx));
        srcstr[0] = deststr[0] = 0;
    }
    sprintf(jsonstr,"{\"txid\":\"%s\",\"tick\":%d,\"src\":\"%s\",\"dest\":\"%s\",\"amount\":\"%s\",\"type\":%d,\"extralen\":%d,\"extra\":\"%s\",\"command\":\"txidrequest\",\"wasm\":1}",txidstr,tx.tick,srcstr,deststr,amountstr(tx.amount),tx.inputType,tx.inputSize,extrastr);
}

void qserver(void)
{
    pthread_t findpeers_thread,dataloop_thread,validate_thread;
    key_t key,respkey;
    char addr[64],spectrumstr[65],qchainstr[65],*msgstr,txidstr[64];
    uint8_t pubkey[32],*ptr,txdata[MAX_INPUT_SIZE*2 + sizeof(struct quheader)];
    int32_t i,j,n,istx,txlen,msgid,txidflag,hexflag,sz,hsz,tick,offset;
    uint32_t utime,newepochflag = 0;
    int32_t year,month,day,seconds,latest;
    int64_t sent,recv;
    long fpos;
    struct qbuffer M,S,C;
    struct quheader H;
    struct subscriber *sp;
    struct addrhash *ap;
    devurandom((uint8_t *)&utime,sizeof(utime));
    srand(utime);
    signal(SIGPIPE, SIG_IGN);
    makefile(QUBIC_MSGPATH);
    key = ftok(QUBIC_MSGPATH, 'Q');
    msgid = msgget(key, 0666 | IPC_CREAT);
    Addresses = (struct addrhash *)calloc(MAXADDRESSES,sizeof(*Addresses));
    RAMQ = (void *)calloc(MAXTICKS,sizeof(*RAMQ));
    printf("key %ld msgid %d RAMQ.%p\n",(long)key,msgid,RAMQ);
    //pthread_mutex_init(&conn_mutex,NULL);
    pthread_mutex_init(&balancechange_mutex,NULL);
    pthread_mutex_init(&txid_mutex,NULL);
    pthread_mutex_init(&addpeer_mutex,NULL);
    pthread_create(&findpeers_thread,NULL,&findpeers,0);
    pthread_create(&dataloop_thread,NULL,&dataloop,0);
    pthread_create(&validate_thread,NULL,&validateloop,0);
    utime = 0;
    latest = 0;
    while ( 1 )
    {
        if ( EXITFLAG != 0 && LATEST_UTIME > EXITFLAG )
            break;
        memset(&M,0,sizeof(M));
        sz = (int32_t)msgrcv(msgid,&M,sizeof(M),0,IPC_NOWAIT);
        //printf("sz.%d sizeH %ld, sz < 0 %d, sz > 8 %d\n",sz,sizeof(H),sz < 0,sz > sizeof(H));
        if ( sz > 0 )
        {
            msgstr = (char *)&M.mesg_text[sizeof(H)];
            tick = txidflag = hexflag = 0;
            if ( strlen(msgstr) == 8 )
                tick = atoi(msgstr);
            else if ( (txidflag= istxid(msgstr)) == 0 )
                hexflag = ishexstr(msgstr);
            if ( tick != 0 || txidflag != 0 || hexflag != 0 )
            {
                if ( txidflag != 0 )
                {
                    txjson(msgstr,(char *)S.mesg_text);
                }
                else if ( hexflag != 0 )
                {
                    txlen = validaterawhex(msgstr,&txdata[sizeof(H)],txidstr);
                    istx = n = 0;
                    if ( txlen >= sizeof(Transaction) )
                    {
                        istx = 1;
                        H = quheaderset(BROADCAST_TRANSACTION,sizeof(H) + txlen);
                        memcpy(txdata,&H,sizeof(H));
                        n = peerbroadcast(txdata,sizeof(H) + txlen);
                    }
                    // if H exists send as command? but below handles it
                    sprintf((char *)S.mesg_text,"{\"istx\":%d,\"txid\":\"%s\",\"broadcast\":%d,\"command\":\"packetrequest\",\"wasm\":1}",istx,txidstr,n);
                }
                else
                {
                    offset = tick - INITIAL_TICK;
                    if ( offset >= 0 && offset < MAXTICKS )
                    {
                        byteToHex(RAMQ[offset].spectrum,spectrumstr,32);
                        byteToHex(RAMQ[offset].qchain,qchainstr,32);
                        S.mesg_type = 1;
                        sprintf((char *)S.mesg_text,"{\"tick\":%d,\"command\":\"tickrequest\",\"qchain\":\"%s\",\"spectrum\":\"%s\"}",tick,qchainstr,spectrumstr);
                    }
                    else
                        sprintf((char *)S.mesg_text,"{\"tick\":%d,\"error\":\"tick not in current epoch\",\"command\":\"tickrequest\",\"initialTick\":%d,\"epoch\":%d}",tick,INITIAL_TICK,EPOCH);
                }
                for (i=0; i<Numsubs; i++)
                {
                    sp = &SUBS[i];
                      if ( sp->respid != 0 )
                    {
                        if ( (sz= (int32_t)msgrcv(sp->respid,&C,sizeof(C),1,MSG_COPY | IPC_NOWAIT)) > 0 )
                        {
                            sp->errs++;
                            if ( (rand() % 100) == 0 )
                                printf("SUBS.%d of %d respid.%d still has message %d errs.%d\n",i,Numsubs,sp->respid,sz,sp->errs);
                            // increase counter and close channel if too many errors
                        }
                        else
                        {
                            msgsnd(sp->respid,&S,strlen((char *)S.mesg_text)+1,IPC_NOWAIT);
                        }
                    }
                }
            }
            else if ( sz >= (int32_t)sizeof(H) )
            {
                sp = 0;
                memcpy(&H,M.mesg_text,sizeof(H));
                hsz = ((H._size[2] << 16) + (H._size[1] << 8) + H._size[0]);
                if ( hsz == (sz - sizeof(uint64_t)) || hsz == (sz - 2*sizeof(uint64_t)) )
                {
                    if ( H._type == REQUEST_ENTITY && hsz == sizeof(H)+sizeof(pubkey) )
                    {
                        ptr = &M.mesg_text[sizeof(H)];
                        memcpy(pubkey,ptr,sizeof(pubkey));
                        ptr += sizeof(pubkey);
                        pubkey2addr(pubkey,addr);
                        if ( hsz == (sz - 2*sizeof(uint64_t)) )
                        {
                            memcpy(&respkey,ptr,sizeof(respkey));
                            ptr += sizeof(respkey);
                            if ( (sp= Subscriberget(respkey,utime)) != 0 )
                            {
                                if ( sp->respid == 0 && (sp->respid= msgget(sp->key,0666 | IPC_CREAT)) != 0 )
                                {
                                    printf("%s %s to respid.%d key %s\n",M.mesg_type == 2 ? "subscribe":"",addr,sp->respid,amountstr(sp->key));
                                    ap = Addresshash(pubkey,utime);
                                    balancechanged(addr,ap,0,0,0);

                                    SubscriberAddr(sp,ap);
                                }
                            }
                        }
                        printf("%s %s\n",addr,sp!=0?"monitoring":"");
                    }
                    else
                    {
                        printf("add to peerQs type.%d hsz.%d (%s),\n",H._type,hsz,&M.mesg_text[sizeof(H)]);
                        //addpeerqueue(H._type,(uint8_t *)&M.mesg_text[sizeof(H)],hsz - sizeof(H));
                        //addpeerqueue(H._type,(uint8_t *)&M.mesg_text[sizeof(H)],hsz - sizeof(H));
                        //addpeerqueue(H._type,(uint8_t *)&M.mesg_text[sizeof(H)],hsz - sizeof(H));
                    }
                    //printf(" H.type %d, sz.%d, type.%ld RECV (hsz.%d)\n",H._type,sz,(long)M.mesg_type,hsz);
                    if ( hsz != 0 && sp != 0 && sp->respid != 0 )
                    {
                        if ( (sz= (int32_t)msgrcv(sp->respid,&C,sizeof(C),1,MSG_COPY | IPC_NOWAIT)) > 0 )
                        {
                            printf("respid.%d still has message %d\n",sp->respid,sz);
                        }
                        else
                        {
                            S.mesg_type = 1;
                            sprintf((char *)S.mesg_text,"qserver got your message of (%d) %s %d %s",hsz,M.mesg_type == 2 ? "subscribe":"",sp->respid,amountstr(sp->key));
                            msgsnd(sp->respid,&S,strlen((char *)S.mesg_text)+1,IPC_NOWAIT);
                        }
                    }
                }
            }
        }
        else usleep(1000);
        utime = set_current_ymd(&year,&month,&day,&seconds);
        if ( utime > LATEST_UTIME )
        {
            LATEST_UTIME = utime;
            if ( newepochflag != 0 && utime > newepochflag+90 && EXITFLAG == 0 )
                EXITFLAG = utime + 30;
        }
        if ( LATEST_TICK > latest )
        {
           // brequest_poll();
            latest = LATEST_TICK;
            printf("update subscribers with %d, VALIDATED.%d HAVETX.%d lag.%d lagv.%d\n",latest,VALIDATED_TICK,HAVE_TXTICK,latest-HAVE_TXTICK,VALIDATED_TICK-HAVE_TXTICK);
            for (i=0; i<Numsubs; i++)
            {
                sp = &SUBS[i];
                S.mesg_type = 1;
                sprintf((char *)S.mesg_text,"{\"tick\":%d,\"wasm\":1,\"command\":\"CurrentTickInfo\",\"initialTick\":%d,\"epoch\":%d}",latest,INITIAL_TICK,EPOCH);
                if ( sp->respid != 0 )
                {
                    if ( (sz= (int32_t)msgrcv(sp->respid,&C,sizeof(C),1,MSG_COPY | IPC_NOWAIT)) > 0 )
                    {
                        sp->errs++;
                        printf("SUBS.%d of %d respid.%d still has message %d errs.%d\n",i,Numsubs,sp->respid,sz,sp->errs);
                        // increase counter and close channel if too many errors
                    }
                    else
                    {
                        msgsnd(sp->respid,&S,strlen((char *)S.mesg_text)+1,IPC_NOWAIT);
                        for (j=0; j<sp->numpubkeys; j++)
                        {
                            if ( (ap= Addresshash(&sp->pubkeys[j * 32],0)) != 0 )
                            {
                                if ( ap->tick >= LATEST_TICK-100 )
                                {
                                    if ( flushaddress(ap,&sent,&recv,&fpos) != 0 )
                                    {
                                        pubkey2addr(ap->entity.publicKey,addr);
                                        if ( sent != 0 || recv != 0 )
                                            balancechanged(addr,ap,sent,recv,fpos);
                                        entityjson((char *)S.mesg_text,addr,ap->entity,ap->tick,ap->merkleroot,sent,recv);
                                        msgsnd(sp->respid,&S,strlen((char *)S.mesg_text)+1+sizeof(uint64_t),IPC_NOWAIT);
                                    }
                                }
                            }
                        }
                        if ( sp->last_validated == 0 )
                            sp->last_validated = VALIDATED_TICK;
                        if ( VALIDATED_TICK > sp->last_validated )
                        {
                            if ( sp->last_validated < INITIAL_TICK )
                                sp->last_validated = INITIAL_TICK-1;
                            for (tick=sp->last_validated+1; tick<VALIDATED_TICK; tick++)
                            {
                                offset = tick - INITIAL_TICK;
                                if ( offset >= 0 && offset < MAXTICKS )
                                {
                                    char spectrumstr[65],qchainstr[65];
                                    byteToHex(RAMQ[offset].spectrum,spectrumstr,32);
                                    byteToHex(RAMQ[offset].qchain,qchainstr,32);
                                    sprintf((char *)S.mesg_text,"{\"tick\":%d,\"wasm\":1,\"command\":\"validated\",\"spectrum\":\"%s\",\"qchain\":\"%s\",\"havetx\":%d}",tick,spectrumstr,qchainstr,HAVE_TXTICK);
                                    msgsnd(sp->respid,&S,strlen((char *)S.mesg_text)+1+sizeof(uint64_t),IPC_NOWAIT);
                                    sp->last_validated = tick;
                                }
                            }
                        }
                    }
                }
            }
            if ( newepochflag == 0 && utime_to_epoch(utime,&seconds) != EPOCH )
            {
                printf("EPOCH change detected %d -> %d\n",EPOCH,utime_to_epoch(utime,&seconds));
                newepoch();
                newepochflag = utime;
            }
            if ( EXITFLAG == 0 && VALIDATED_TICK != 0 && LATEST_UTIME > PROGRESSTIME+300 )
            {
                printf("tick.%d stuck for %d seconds, start exit %d\n",LATEST_TICK,LATEST_UTIME - PROGRESSTIME,EXITFLAG);
                EXITFLAG = LATEST_UTIME + 30;
                qpurge(VALIDATED_TICK+1);
                if ( HAVE_TXTICK != 0 )
                    qpurge(HAVE_TXTICK+1);
            }
        }
    }
    msgctl(msgid,IPC_RMID,NULL);
    sleep(15);
    printf("qserver exiting\n");
}


#define HAVEMAIN
int main(int argc, const char * argv[])
{
    uint32_t utime;
    int32_t year,month,day,seconds;
    printf("%s Qsize %ld\n",argv[0],sizeof(Qchain));
    utime = set_current_ymd(&year,&month,&day,&seconds);
    EPOCH = utime_to_epoch(utime,&seconds);
    if ( argc == 2 )
        makevanity((char *)argv[1]);
    init_txids();
    RAMQ = (void *)calloc(MAXTICKS,sizeof(*RAMQ));
    makedir((char *)"epochs");
    makedir((char *)"subs");
    makedir((char *)"addrs");
    newepoch();
    qserver();
    return(0);
}


/*

 N possible txid between entity changes, need all money txid in tick
 
 websockets comms: limit packets
 
 qclient authentication after initial login
 prevent broadcasting responses
*/
