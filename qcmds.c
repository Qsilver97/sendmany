

/*
struct qrequest *qrequest_tickinfo(void)
{
    return(qrequest_add(0,REQUEST_CURRENT_TICK_INFO,0,0));
}

struct qrequest *qrequest_computors(void)
{
    return(qrequest_add(0,REQUEST_COMPUTORS,0,0));
}

struct qrequest *qrequest_tickdata(int32_t tick)
{
    return(qrequest_add(0,REQUEST_TICK_DATA,(uint8_t *)&tick,sizeof(tick)));
}

struct qrequest *qrequest_entity(uint8_t pubkey[32])
{
    return(qrequest_add(0,REQUEST_ENTITY,pubkey,32));
}

struct qrequest *qrequest_issued(uint8_t pubkey[32])
{
    return(qrequest_add(0,REQUEST_ISSUED_ASSETS,pubkey,32));
}

struct qrequest *qrequest_possessed(uint8_t pubkey[32])
{
    return(qrequest_add(0,REQUEST_POSSESSED_ASSETS,pubkey,32));
}

struct qrequest *qrequest_owned(uint8_t pubkey[32])
{
    return(qrequest_add(0,REQUEST_OWNED_ASSETS,pubkey,32));
}

struct qrequest *qrequest_quorumtick(int32_t tick)
{
    RequestedQuorumTick R;
    memset(&R,0,sizeof(R));
    R.tick = tick;
    return(qrequest_add(REQUEST_QUORUMTICK,0,(uint8_t *)&R,sizeof(R)));
}

struct qrequest *qrequest_ticktransactions(int32_t tick)
{
    RequestedTickTransactions R;
    memset(&R,0,sizeof(R));
    R.tick = tick;
    return(qrequest_add(0,REQUEST_TICK_TRANSACTIONS,(uint8_t *)&R,sizeof(R)));
}

struct qrequest *qrequest_sendtx(uint8_t *txdata,int32_t txlen)
{
    return(qrequest_add(0,BROADCAST_TRANSACTION,txdata,txlen));
}
*/

CurrentTickInfo getTickInfoFromNode(const char *nodeIp,int32_t nodePort)
{
    struct quheader R;
    CurrentTickInfo result,*rp;
    int32_t sock;
    uint8_t buf[4096];
    memset(&result,0,sizeof(result));
    if ( (sock= myconnect(nodeIp,nodePort)) < 0 )
        return(result);
    if ( (rp= reqresponse(buf,sizeof(buf),sock,REQUEST_CURRENT_TICK_INFO,(uint8_t *)&R,sizeof(R),RESPOND_CURRENT_TICK_INFO)) != 0 )
        memcpy(&result,rp,sizeof(result));
    close(sock);
    return result;
}

RespondedEntity getBalance(const char *nodeIp,const int nodePort,const uint8_t *pubkey,uint8_t *merkleroot)
{
    struct EntityRequest ER;
    RespondedEntity result,*rp;
    int32_t sock;
    uint8_t buf[4096];
    memset(&result,0,sizeof(result));
    if ( (sock= myconnect(nodeIp,nodePort)) < 0 )
        return(result);
    memcpy(ER.pubkey,pubkey,sizeof(ER.pubkey));
    if ( merkleroot != 0 )
        memset(merkleroot,0,32);
    if ( (rp= reqresponse(buf,sizeof(buf),sock,REQUEST_ENTITY,(uint8_t *)&ER,sizeof(ER),RESPOND_ENTITY)) != 0 )
    {
        memcpy(&result,rp,sizeof(result));
        if ( merkleroot != 0 )
        {
            memset(merkleroot,0,32);
            merkleRoot(SPECTRUM_DEPTH,rp->spectrumIndex,(uint8_t *)&rp->entity,sizeof(rp->entity),&rp->siblings[0][0],merkleroot);
            char hexstr[65];
            byteToHex(merkleroot,hexstr,32);
            hexstr[64] = 0;
            printf("getBalance tick.%d merkle.%s\n",rp->tick,hexstr);
        }
    }
    close(sock);
    return result;
}

void sendrawtransaction(const char *ipaddr,uint16_t port,const char *rawhex)
{
    int32_t sock,datalen;
    uint8_t reqbuf[4096];//,buf[4096],*rp;
    if ( (sock= myconnect(ipaddr,port)) < 0 )
    {
        printf("Error getting connection %s for sendrawtransaction\n",ipaddr);
        return;
    }
    datalen = (int32_t)strlen(rawhex) / 2;
    hexToByte(rawhex,&reqbuf[sizeof(struct quheader)],datalen);
    *(struct quheader *)reqbuf = quheaderset(BROADCAST_TRANSACTION,datalen+sizeof(struct quheader));
    sendbuffer(sock,reqbuf,datalen+sizeof(struct quheader));

    /*if ( (rp= reqresponse(buf,sizeof(buf),sock,BROADCAST_TRANSACTION,reqbuf,datalen+sizeof(struct quheader),EXCHANGE_PUBLIC_PEERS)) != 0 )
    {
        //for (int i=0; i<16; i++)
        //    printf("%02x",rp[i]);
        //printf(" respnse\n");
    }*/
    close(sock);
    return;
}

////////////////// wrappers


int32_t timebasedepoch(int32_t *startp,int32_t *latestp)
{
    uint32_t utime;
    int32_t i,year,month,day,seconds,epoch;
    utime = set_current_ymd(&year,&month,&day,&seconds);
    epoch = utime_to_epoch(utime,&seconds);
    for (i=0; i<3; i++)
    {
        CurrentTickInfo info = getTickInfoFromNode(randpeer(0),DEFAULT_NODE_PORT);
        printf("Epoch %d:%d, days %d, hours %d, seconds %d vs %d/%d start.%d\n",epoch,info.epoch,seconds/(3600 * 24),(seconds % (3600 * 24))/3600,seconds%3600,info.epoch,info.tick,info.initialTick);
        if ( info.tick > 0 && info.initialTick > 12000000 ) //epoch == info.epoch && 
        {
            *startp = info.initialTick;
            *latestp = info.tick;
            if ( info.tick > LATEST_TICK )
                LATEST_TICK = info.tick;
            return(epoch);
        }
    }
    *startp = 0;
    *latestp = 0;
    return(0);
}
