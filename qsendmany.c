
//#define RECLAIMDEST // reclaims funds in dest.yyy test addresses

int64_t reclaimbalance(int64_t *totalp,char *iFAN,char *addr,uint8_t subseed[32],char *dest,int32_t starttick)
{
    uint8_t pubkey[32],digest[32];
    char rawhex[4096],txhash[64];
    int64_t balance;
    addr2pubkey(addr,pubkey);
    balancetickhash(pubkey,starttick);
    if ( (balance= waitforbalance(0,pubkey,starttick)) != 0 )
    {
        (*totalp) += balance;
        create_rawtransaction(rawhex,txhash,digest,subseed,dest,balance,0,"");
        printf("reclaim %s %s %s %s\n",iFAN,addr,amountstr(balance),rawhex);
        sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,rawhex);
    }
    return(balance != 0);
}

int reclaim(char *origseed,int32_t numaddrs,int32_t starttick)
{
    uint8_t subseed[32],pubkey[32];
    int32_t i,nonz = 0;
    int64_t reclaimed = 0;
    char origaddr[64],addr[64],iFAN[6];
#ifdef RECLAIMDEST
    char destiFAN[64];
#endif
    fanout_subseed(origseed,(char *)"",subseed,origaddr);
    //printf("%s -> %s\n",origseed,origaddr);
    fanout_subseed(origseed,(char *)"Z",subseed,addr);
    //if ( reclaimbalance(&reclaimed,(char *)"Z",addr,subseed,origaddr,starttick) != 0 )
    //    nonz++;
    if ( numaddrs > 0 )
    {
        for (i=0; i<25; i++)
        {
            calc_iFAN(iFAN,i,25);
            fanout_subseed(origseed,iFAN,subseed,addr);
            addr2pubkey(addr,pubkey);
            balancetickhash(pubkey,starttick);
        }
        for (i=0; i<25*25; i++)
        {
            if ( i/FAN >= numaddrs )
                break;
            calc_iFAN(iFAN,i,25*25);
            fanout_subseed(origseed,iFAN,subseed,addr);
            addr2pubkey(addr,pubkey);
            balancetickhash(pubkey,starttick);
        }
#ifdef RECLAIMDEST
        for (i=0; i<25*25*25; i++)
        {
            if ( i >= numaddrs )
                break;
            calc_iFAN(iFAN,i,25*25*25);
            sprintf(destiFAN,"dest.%s",iFAN);
            fanout_subseed(origseed,destiFAN,subseed,addr);
            addr2pubkey(addr,pubkey);
            balancetickhash(pubkey,starttick);
        }
#endif
        printf("add to hashtable\n");
        for (i=0; i<25; i++)
        {
            calc_iFAN(iFAN,i,25);
            fprintf(stderr,"%s ",iFAN);
            fanout_subseed(origseed,iFAN,subseed,addr);
            if ( reclaimbalance(&reclaimed,iFAN,addr,subseed,origaddr,starttick) != 0 )
                nonz++;
        }
        printf("level 1\n");
        for (i=0; i<25*25; i++)
        {
            if ( i/FAN >= numaddrs )
                break;
            calc_iFAN(iFAN,i,25*25);
            fprintf(stderr,"%s ",iFAN);
            fanout_subseed(origseed,iFAN,subseed,addr);
            if ( reclaimbalance(&reclaimed,iFAN,addr,subseed,origaddr,starttick) != 0 )
                nonz++;
        }
#ifdef RECLAIMDEST
        for (i=0; i<25*25*25; i++)
        {
            if ( i >= numaddrs )
                break;
            calc_iFAN(iFAN,i,25*25*25);
            sprintf(destiFAN,"dest.%s",iFAN);
            fprintf(stderr,"%s ",destiFAN);
            fanout_subseed(origseed,destiFAN,subseed,addr);
            if ( reclaimbalance(&reclaimed,destiFAN,addr,subseed,origaddr,starttick) != 0 )
                nonz++;
        }
#endif
        printf("level 2\n");
    }
    printf("issued reclaims from %d addrs for %s\n",nonz,amountstr(reclaimed));
    return(nonz);
}

int sendmany(char *origseed,char *fname,int32_t autogenflag)
{
    int32_t i,n,starttick,endtick,numerrors = 0;
    int64_t paid = 0;
    uint8_t subseed[32];
    char dest[64],firstaddr[64];
    struct Fanout *fan;
    CurrentTickInfo cur;
    TXQ_PAUSE = 1;
    cur = getTickInfoFromNode(randpeer(0),DEFAULT_NODE_PORT);
    printf("T.%ld dur.%d epoch.%d tick.%d aligned.%d misaligned.%d initialTick.%d\n",sizeof(Transaction),cur.tickDuration,cur.epoch,cur.tick,cur.numberOfAlignedVotes,cur.numberOfMisalignedVotes,cur.initialTick);

    while ( (starttick= getlatest()) <= 1 )
    {
        printf("waiting for starttick\n");
        sleep(1);
    }
    if ( (fan= fanout_create(origseed,fname,autogenflag)) != 0 )
    {
        //reclaim(origseed,fan->numdests,starttick);
        fanout_send(fan);
        fanout_subseed(origseed,(char *)"Z",subseed,firstaddr);
        printf("\nSEND TOTAL REQUIRED FOR SENDMANY INCLUDING FEES: %s to %s\nAll data logged in %s\n",amountstr(fan->total),firstaddr,fan->txidsdir);
        TXQ_PAUSE = 0;
        printf("numdests.%d depth.%d total %s sum1 %s, sum2 %s, sum3 %s\n",fan->numdests,fan->depth,amountstr(fan->total),amountstr2(fan->depth1sum),amountstr3(fan->depth2sum),amountstr4(fan->depth3sum));
        while ( (n= txq_queue_numactive()) > 0 )
        {
            if ( (rand() % 20) == 0 )
                printf("num remaining in TXQ.%d\n",n);
            sleep(1);
        }
    }
    else
        printf("error creating fanout from %s\n",fname);
    while ( (endtick= getlatest()) <= 1 )
    {
        printf("waiting for endtick\n");
        sleep(1);
    }
    printf("elapsed %d ticks %d %d, generating report\n",endtick - starttick,starttick,endtick);
    if ( Paymentamounts[0] != 0 )
    {
        for (i=0; i<fan->numdests; i++)
            balancetickhash(Paymentpubkeys[i],1);
        for (i=0; i<fan->numdests; i++)
        {
            Endingbalances[i] = waitforbalance(0,Paymentpubkeys[i],endtick);
            if ( (i % 1000) == 0 )
                fprintf(stderr,".");
        }
        for (i=0; i<fan->numdests; i++)
        {
            pubkey2addr(Paymentpubkeys[i],dest);
            if ( Paymentamounts[i] - (Endingbalances[i] - Startingbalances[i]) != 0 )
            {
                printf("paid %s: %s error %s\n",dest,amountstr(Endingbalances[i] - Startingbalances[i]),amountstr2(Paymentamounts[i] - (Endingbalances[i] - Startingbalances[i])));
                numerrors++;
            }
            else paid += (Endingbalances[i] - Startingbalances[i]);
        }
    }
    reclaim(origseed,fan->numdests,LATEST_TICK);
    endtick = getlatest();
    printf("total elapsed %d ticks, numerror.%d paid %s totalcost %s numsendmany.%d numpaid.%d\n",endtick - starttick,numerrors,amountstr(paid),amountstr2(fan->total),txq_Numtx,fan->numdests);
    free(fan);
    return(0);
}
