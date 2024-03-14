//
//  txqueue.cpp
//  fanout
//

int32_t txq_Numtx,CUR_tick,NUMCUR_tick,LATEST_NUMACTIVE,NUMFAILS,NUMGOOD,TXQ_PAUSE = 1;

struct txq_done
{
    char txhash[64],rawhex[MAX_INPUT_SIZE*3 + 1],padc;
    int32_t txtick;
};

struct txq_item
{
    uint8_t senderseed[32],senderpubkey[32],txdigest[32];
    struct txq_done confirmed;
    struct pubkeypay payments;
    pthread_t txsend_thread,txcheck_thread;
    char senderaddr[64],txidsdir[512],pendingtxid[64],datastr[MAX_INPUT_SIZE*2 + 1],padc;
    int32_t datalen,pendingtick,pendingcheck,epochstart,numpayments,sent,starti,depth,createdtick;
    int64_t waitforbalance,required,origbalance,origsent;
    uint64_t sendflags;
    struct txq_item *waitfor;
} *TX_Q[MAXFANS];

struct balancetick
{
    uint8_t pubkey[32];
    int64_t balance,outgoing,balances[3],outgoings[3];
    int32_t tick,ticks[3];
    uint32_t ipaddrs[3];
    int32_t index,nexthashi,mintick,extra0,extra1;
} *Balances,*FIRST_bp;
int32_t txq_Numbalances,txq_HASHSIZE;

struct balancetick *balancetickhash(uint8_t pubkey[32],int32_t addflag)
{
    static struct balancetick *prevbp;
    int32_t i,hashi,tick;
    struct balancetick *bp;
    pthread_mutex_lock(&txq_mutex);
    hashi = *(uint32_t *)pubkey % txq_HASHSIZE;
    for (i=0; i<txq_HASHSIZE; i++)
    {
        bp = &Balances[(hashi + i) % txq_HASHSIZE];
        if ( bp->index == 0 )
        {
            if ( addflag == 0 )
            {
                pthread_mutex_unlock(&txq_mutex);
                return(bp);
            }
            else
            {
                memset(bp,0,sizeof(*bp));
                memcpy(bp->pubkey,pubkey,sizeof(bp->pubkey));
                bp->index = ++txq_Numbalances;
                tick = (addflag == 1) ? LATEST_TICK : addflag;
                if ( tick > bp->mintick )
                    bp->mintick = tick;
                if ( prevbp != 0 )
                    prevbp->nexthashi = (hashi + i) % txq_HASHSIZE;
                else FIRST_bp = bp;
                prevbp = bp;
                char addr[64];
                pubkey2addr(pubkey,addr);
                //printf("add %s %d -> %d\n",addr,bp->index,(hashi + i) % txq_HASHSIZE);
                pthread_mutex_unlock(&txq_mutex);
                return(bp);
            }
        }
        else if ( memcmp(bp->pubkey,pubkey,sizeof(bp->pubkey)) == 0 )
        {
            tick = (addflag == 1) ? LATEST_TICK : addflag;
            if ( tick > bp->mintick )
                bp->mintick = tick;
            pthread_mutex_unlock(&txq_mutex);
            return(bp);
        }
    }
    pthread_mutex_unlock(&txq_mutex);
    printf("Balances HASH table full!\n");
    return(0);
}

int64_t waitforbalance(int64_t *outgoingp,uint8_t pubkey[32],int32_t mintick)
{
    struct balancetick *bp;
    int32_t i,maxtick;
    if ( (bp= balancetickhash(pubkey,mintick)) == 0 )
    {
        printf("cannot add to Balances?\n");
        while ( 1 )
            sleep(1);
    }
    else if ( mintick != 0 )
    {
        if ( bp->mintick < mintick )
        {
            bp->mintick = mintick;
            memset(bp->ipaddrs,0,sizeof(bp->ipaddrs));  // triggers peers to request balance
        }
        while ( mintick > 0 && bp->tick < mintick )
        {
            usleep(1000);
            maxtick = 0;
            for (i=0; i<(sizeof(bp->ipaddrs)/sizeof(*bp->ipaddrs)); i++)
            {
                if ( bp->ipaddrs[i] == 0 )
                    break;
                if ( bp->ticks[i] > maxtick )
                {
                    maxtick = bp->ticks[i];
                    bp->tick = maxtick;
                    bp->balance = bp->balances[i];
                    bp->outgoing = bp->outgoings[i];
                    if ( outgoingp != 0 )
                        *outgoingp = bp->outgoing;
                }
                else if ( bp->ticks[i] == maxtick )
                {
                    if ( bp->balances[i] != bp->balance )
                        printf("ipaddr.%d: balance mismatch %s vs %s\n",i,amountstr(bp->balance),amountstr2(bp->balances[i]));
                }
            }
        }
    }
    if ( outgoingp != 0 && bp->outgoing != 0 )
        *outgoingp = bp->outgoing;
    return(bp->balance);
}

void *txq_peerloop(void *_ipbytes)
{
    struct balancetick *bp;
    struct txq_item *qp;
    struct EntityRequest ER;
    struct quheader H;
    char ipaddr[16],addr[64];
    uint64_t peermask;
    uint32_t prevutime,iphash;
    int32_t prevtick,sock,sz,datalen,counter,modval,recvbyte,ptr,bufsize = 4 * 1024 * 1024;
    static uint8_t *buf;
    uint8_t reqbuf[MAX_INPUT_SIZE*2],*ipbytes = _ipbytes;
    prevtick = 0;
    counter = 0;
    prevutime = 0;
    peermask = 1LL << (rand() % 64);
    iphash = *(uint32_t *)ipbytes;
    if ( buf == 0 )
        buf = (uint8_t *)calloc(1,bufsize);
    sprintf(ipaddr,"%d.%d.%d.%d",ipbytes[0],ipbytes[1],ipbytes[2],ipbytes[3]);
    printf("start peer loop.%s\n",ipaddr);
    if ( (sock= myconnect(ipaddr,DEFAULT_NODE_PORT)) < 0 )
    {
        printf("txq_peerloop error connecting to %s\n",ipaddr);
        return(0);
    }
    printf(">>>>>>>>>>>>>>>>>>> start recvloop sock.%d ipaddr %d.%d.%d.%d\n",sock,ipbytes[0],ipbytes[1],ipbytes[2],ipbytes[3]);
    while ( 1 )
    {
        uint32_t utime;
        int32_t i,n,year,month,day,seconds,issued;
        utime = set_current_ymd(&year,&month,&day,&seconds);
        if ( utime > prevutime ) // optimization: use millisecond offset*peerid
        {
            //printf("%s new utime.%u %d\n",ipaddr,utime,LATEST_TICK);
            counter++;
            modval = 1 + (txq_Numbalances / MAXENTITY_PERSECOND);
            prevutime = utime;
            LATEST_UTIME = utime;
            H = quheaderset(REQUEST_CURRENT_TICK_INFO,sizeof(H));
            sock = socksend(ipaddr,sock,(uint8_t *)&H,sizeof(H));
            if ( (bp= FIRST_bp) != 0 )
            {
                n = 0;
                issued = 0;
                while ( bp->index != 0 )
                {
                    n++;
                    if ( /*((n + counter) % modval) == 0 &&*/ issued < MAXENTITY_PERSECOND && bp->tick < bp->mintick && (LATEST_TICK == 0 || LATEST_TICK >= bp->mintick) )
                    {
                        for (i=0; i<(sizeof(bp->ipaddrs)/sizeof(*bp->ipaddrs)); i++)
                        {
                            if ( bp->ipaddrs[i] == 0 || (bp->ipaddrs[i] == iphash && bp->ticks[i] < bp->mintick) )
                            {
                                bp->ipaddrs[i] = iphash;
                                memset(ER.pubkey,0,sizeof(ER.pubkey));
                                memcpy(ER.pubkey,bp->pubkey,sizeof(ER.pubkey));
                                ER.H = quheaderset(REQUEST_ENTITY,sizeof(ER));
                                sock = socksend(ipaddr,sock,(uint8_t *)&ER,sizeof(ER));
                                pubkey2addr(ER.pubkey,addr);
                                issued++;
                                if ( (issued % 100) == 0 )
                                {
                                    H = quheaderset(REQUEST_CURRENT_TICK_INFO,sizeof(H));
                                    sock = socksend(ipaddr,sock,(uint8_t *)&H,sizeof(H));
                                }
                                //printf("%s iphash.%lx %d req.%d %s ticks[] %d < %d\n",ipaddr,(long)peermask,i,bp->index,addr,bp->ticks[i],bp->mintick);
                                break;
                            }
                            else if ( bp->ipaddrs[i] == iphash )
                                break;
                        }
                    }
                    bp = &Balances[bp->nexthashi];
                }
                if ( n != txq_Numbalances )
                    printf("%s unexpected n.%d vs numbalances.%d\n",ipaddr,n,txq_Numbalances);
                prevtick = LATEST_TICK;
            }
            for (i=0; i<txq_Numtx; i++)
            {
                if ( (qp= TX_Q[i]) != 0 && (qp->sendflags & peermask) != 0 )
                {
                    datalen = (int32_t)strlen(qp->confirmed.rawhex) / 2;
                    hexToByte(qp->confirmed.rawhex,&reqbuf[sizeof(struct quheader)],datalen);
                    *(struct quheader *)reqbuf = quheaderset(BROADCAST_TRANSACTION,sizeof(struct quheader) + datalen);
                    sock = socksend(ipaddr,sock,reqbuf,sizeof(struct quheader) + datalen);
                    qp->sendflags ^= peermask;
                }
            }
        }
        if ( (recvbyte= receiveall(sock,buf,bufsize)) > 0 )
        {
            ptr = 0;
            sz = 1;
            //printf("received %d\n",recvbyte);
            while ( ptr < recvbyte && sz != 0 && sz < recvbyte )
            {
                memcpy(&H,&buf[ptr],sizeof(H));
                sz = ((H._size[2] << 16) + (H._size[1] << 8) + H._size[0]);
                if ( H._type == RESPOND_CURRENT_TICK_INFO )
                {
                    CurrentTickInfo cur;
                    memcpy(&cur,&buf[ptr + sizeof(H)],sizeof(cur));
                    if ( cur.tick > LATEST_TICK )
                    {
                        LATEST_TICK = cur.tick;
                        //printf("%s T.%ld dur.%d epoch.%d tick.%d aligned.%d misaligned.%d initialTick.%d\n",ipaddr,sizeof(Transaction),cur.tickDuration,cur.epoch,cur.tick,cur.numberOfAlignedVotes,cur.numberOfMisalignedVotes,cur.initialTick);
                    }
                }
                else if ( H._type == RESPOND_ENTITY )
                {
                    RespondedEntity E;
                    memcpy(&E,&buf[ptr + sizeof(H)],sizeof(E));
                    pubkey2addr(E.entity.publicKey,addr);
                    if ( (bp= balancetickhash(E.entity.publicKey,0)) != 0 )
                    {
                        for (i=0; i<(sizeof(bp->ipaddrs)/sizeof(*bp->ipaddrs)); i++)
                        {
                            if ( bp->ipaddrs[i] == iphash )
                            {
                                bp->ticks[i] = E.tick;
                                bp->balances[i] = E.entity.incomingAmount - E.entity.outgoingAmount;
                                bp->outgoings[i] = E.entity.outgoingAmount;
                                //printf("%s %d %d: %s.%d %s\n",ipaddr,i,bp->index,addr,E.tick,amountstr(E.entity.incomingAmount - E.entity.outgoingAmount));
                                if ( bp->ticks[i] > bp->tick )
                                {
                                    bp->outgoing = bp->outgoings[i];
                                    bp->balance = bp->balances[i];
                                    bp->tick = bp->ticks[i];
                                }
                                break;
                            }
                        }
                    }
                    else printf("%s Unexpected balance data for %s arrived\n",ipaddr,addr);
                }
                else if ( H._type == EXCHANGE_PUBLIC_PEERS )
                {
                    uint8_t ipaddrs[4][4];
                    memcpy(ipaddrs,&buf[ptr + sizeof(H)],sizeof(ipaddrs));
                    //for (i=0; i<4; i++)
                    //    printf("%d.%d.%d.%d ",ipaddrs[i][0],ipaddrs[i][1],ipaddrs[i][2],ipaddrs[i][3]);
                    //printf("peer ipaddrs %s\n",ipaddr);
                }
                else
                    printf("%s txq_peerloop received %d, size.%d unhandled type.%d\n",ipaddr,recvbyte,sz,H._type);

                ptr += sz;
            }
        } else usleep(10000);
        usleep(10000 + (rand() % 10000));
    }
    close(sock);
    return(0);
}

void *txq_send(void *_qp) // originally in dedicated thread, but now polled
{
    struct txq_item *qp = (struct txq_item *)_qp;
    int64_t required = SENDMANYFEE;
    int32_t i;
    qp->pendingtick = LATEST_TICK + TICKOFFSET;
    for (i=0; i<qp->numpayments; i++)
        required += qp->payments.amounts[i];
    qp->required = required;
    while ( (qp->origbalance= waitforbalance(&qp->origsent,qp->senderpubkey,qp->createdtick)) < required )
    {
        if ( (rand() % 5) == 0 )
            printf("%s %s does not have %s\n",qp->senderaddr,amountstr2(qp->origbalance),amountstr(required));
        usleep(100000);
    }
    qp->datalen = sizeof(qp->payments);
    qp->sent++;
    printf("%s txq_send %s numpayments.%d origbalance.%s required.%s expected.%s pendingtick.%d sent.%d\n",qp->senderaddr,amountstr(required),qp->numpayments,amountstr2(qp->origbalance),amountstr3(qp->required),amountstr4(qp->origbalance - required),qp->pendingtick,qp->sent);
    pthread_mutex_lock(&txq_sendmutex);
    qp->pendingtick = LATEST_TICK + TICKOFFSET;
    if ( qp->pendingtick < CUR_tick )
        qp->pendingtick = CUR_tick;
    else if ( qp->pendingtick > CUR_tick )
    {
        CUR_tick = qp->pendingtick;
        NUMCUR_tick = 0;
    }
    //printf("CURtick.%d n.%d pending.%d\n",CUR_tick,NUMCUR_tick,qp->pendingtick);
    if ( qp->pendingtick == CUR_tick )
    {
        NUMCUR_tick++;
        if ( NUMCUR_tick > MAXSENDMANY_PERTICK )
        {
            //printf("%d has %d, incr to %d\n",CUR_tick,NUMCUR_tick,CUR_tick+1);
            NUMCUR_tick = 1;
            CUR_tick++;
            qp->pendingtick = CUR_tick;
        }
    }
    pthread_mutex_unlock(&txq_sendmutex);

    create_rawsendmany(qp->confirmed.rawhex,qp->pendingtxid,qp->txdigest,qp->senderseed,qp->pendingtick,&qp->payments,qp->numpayments);
    qp->sendflags = -1;
    
    //sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,qp->confirmed.rawhex);
#ifndef TESTNET
    //sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,qp->confirmed.rawhex);
    //sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,qp->confirmed.rawhex);
#endif

    waitforbalance(0,qp->senderpubkey,0);
    return(0);
}

void txq_check(struct txq_item *qp)
{
    int64_t balance,outgoing;
    char fname[1024];
    FILE *fp;
    //printf("txq_check numpayments.%d datalen.%d %s %s\n",qp->numpayments,qp->datalen,qp->datastr,qp->senderaddr);
    balance = waitforbalance(&outgoing,qp->senderpubkey,qp->pendingtick+1);
    if ( outgoing != (qp->origsent + qp->required) )
    {
        printf("%s %s failed still has %s orig.%s required.%s sent.%s\n",qp->senderaddr,qp->pendingtxid,amountstr(balance),amountstr2(qp->origbalance),amountstr3(qp->required),amountstr4(outgoing - qp->origsent));
        memset(qp->pendingtxid,0,sizeof(qp->pendingtxid));
        qp->pendingtick = 0;
        qp->pendingcheck = 0;
        NUMFAILS++;
    }
    else
    {
        NUMGOOD++;
        strcpy(qp->confirmed.txhash,qp->pendingtxid);
        qp->confirmed.txtick = qp->pendingtick;
        sprintf(fname,"%s%c%s",qp->txidsdir,dir_delim(),qp->confirmed.txhash);
        printf("%s starti.%d found %s pend.%d latest.%d\n",qp->senderaddr,qp->starti,qp->pendingtxid,qp->pendingtick,LATEST_TICK);
        if ( (fp= fopen(fname,"w")) != 0 )
        {
            fprintf(fp,"%d,%d,%d\n%s\n%s\n%s\n",qp->starti,qp->depth,FANDEPTH,qp->senderaddr,qp->confirmed.txhash,qp->confirmed.rawhex);
            fclose(fp);
        }
    }
}

void *txq_loop(void *threads)
{
    uint32_t lastutime = 0;
    int32_t i,dispflag,numpending,iter = 0;
    struct txq_item *qp;
    struct balancetick *bp;
    printf("start txq_loop LATEST_TICK.%d\n",LATEST_TICK);
    while ( 1 )
    {
        if ( TXQ_PAUSE != 0 )
            sleep(1);
        dispflag = (LATEST_UTIME > lastutime);
        //printf("curtick.%d latest.%d\n",curtick,SendManyLatest);
        for (numpending=i=0; i<txq_Numtx; i++)
        {
            if ( (qp= TX_Q[i]) == 0 )
            {
                printf("null TX_Q[%d]\n",i);
                continue;
            }
            if ( qp->confirmed.txtick != 0 )
            {
                //printf("confirmed TX_Q[%d]\n",i);
                continue;
            }
            if ( qp->waitfor != 0 && qp->waitfor->confirmed.txtick == 0 )
            {
                //printf("waiting for waitfor TX_Q[%d]\n",i);
                continue;
            }
            else if ( qp->pendingtick == 0 )
            {
                if ( (bp= balancetickhash(qp->senderpubkey,1)) != 0 && bp->balance >= qp->required && bp->tick > qp->createdtick )
                    txq_send(qp);
            }
            else
            {
                numpending++;
                if ( qp->pendingtick != 0 && qp->confirmed.txtick == 0 && LATEST_TICK >= qp->pendingtick && qp->pendingcheck == 0 && qp->pendingtxid[0] != 0 )
                {
                    if ( (bp= balancetickhash(qp->senderpubkey,0)) != 0 )
                    {
                        if ( bp->tick > qp->pendingtick )
                        {
                            qp->pendingcheck = qp->pendingtick;
                            txq_check(qp);
                        }
                        else balancetickhash(qp->senderpubkey,qp->pendingtick+1);
                    }
                }
            }
            if ( dispflag != 0 && qp->pendingtick != 0 )
            {
                int64_t balance;
                printf("now.%d txQ[%d]: pendingtick.%d confirmed.%d sent.%d pendingcheck.%d numpending.%d G.%d F.%d ",LATEST_TICK,i,qp->pendingtick,qp->confirmed.txtick,qp->sent,qp->pendingcheck,numpending,NUMGOOD,NUMFAILS);
                if ( i == 0 )
                {
                    balance = waitforbalance(0,qp->senderpubkey,0);
                    if ( qp->required > balance )
                        printf("send %s to %s",amountstr(qp->required - balance),qp->senderaddr);
                }
                printf("\n");
                lastutime = LATEST_UTIME + 5;
                dispflag = 0;
            }
            iter++;
        }
        usleep(10000);
    }
    printf("end txq_loop\n");
    return(0);
}

int32_t txq_queue_numactive(void)
{
    int32_t i,numactive = 0;
    struct txq_item *qp;
    for (i=0; i<txq_Numtx; i++)
    {
        if ( (qp= TX_Q[i]) == 0 )
            continue;
        if ( qp->confirmed.txtick == 0 )
            numactive++;
    }
    LATEST_NUMACTIVE = numactive;
    return(numactive);
}

struct txq_item *txq_queuetx(char *txidsdir,uint8_t subseed[32],struct pubkeypay *payments,int32_t n,int32_t epochstart,struct txq_item *waitfor,int32_t starti,int32_t depth)
{
    struct txq_item *qp;
    uint8_t privatekey[32];
    qp = (struct txq_item *)calloc(1,sizeof(*qp));
    strncpy(qp->txidsdir,txidsdir,sizeof(qp->txidsdir)-1);
    memcpy(qp->senderseed,subseed,sizeof(qp->senderseed));
    getPrivateKeyFromSubSeed(subseed,privatekey);
    getPublicKeyFromPrivateKey(privatekey,qp->senderpubkey);
    pubkey2addr(qp->senderpubkey,qp->senderaddr);
    qp->payments = *payments;
    qp->numpayments = n;
    qp->epochstart = epochstart;
    qp->waitfor = waitfor;
    qp->starti = starti;
    qp->createdtick = LATEST_TICK;
    qp->depth = depth;
    TX_Q[txq_Numtx] = qp;
    txq_Numtx++;
    //printf("added tx to queue.%d\n",txq_Numtx);
    return(qp);
}

uint32_t getlatest(void)
{
    while ( LATEST_TICK <= 1 )
        sleep(1);
    return(LATEST_TICK);
}
