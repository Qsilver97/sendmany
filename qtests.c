

void testsend(char *seed55,char *destaddr,int64_t amount)
{
    char rawhex[4096],txidstr[64],senderaddr[64],iFAN[6];
    uint8_t txdigest[32],subseed[32];
    memset(iFAN,0,sizeof(iFAN));
    getSubseedFromSeed((uint8_t *)seed55,subseed);
    if ( create_rawtransaction(rawhex,txidstr,txdigest,subseed,destaddr,amount,0,"") == 0 )
    {
        fanout_subseed(seed55,iFAN,subseed,senderaddr);
        printf("created transaction send from %s to %s %ld, %s rawhex.%s\n",senderaddr,destaddr,(long)amount,txidstr,rawhex);
        sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,rawhex);
    } else printf("invalid destination %s\n",destaddr);
}

void testautofix(void)
{
    printf("checksums %d %d\n",
           checkSumIdentity((char* )"XYUGINXMIDNHODNHTRUSNPNXLBSBVHFXOJDFFRXPPCLSWZHPHYWAMPXCVIOD"),
           checkSumIdentity((char*) "XYUGINXMIFNHODNHTRUSNPNXLBSBVHFXOJDFFRXPPCLSWZHPHYWAMPXCVIOD"));
    testsend((char *)"test",(char *)"XYUGINXMIDNHODNHTRUSNPNXLBSBVHFXOJDFFRXPPCLSWZHPHYWAMPXCVIOD",1);
}

int32_t testaddr2pubkey(void)
{
    char addr[64];
    int32_t i;
    uint8_t pubkey[32],checkpub[32],subseed[32],privkey[32];
    for (i=0; i<1000000; i++)
    {
        devurandom(subseed,32);
        getPrivateKeyFromSubSeed(subseed,privkey);
        getPublicKeyFromPrivateKey(privkey,pubkey);
        getIdentityFromPublicKey(pubkey,addr,false);
        if ( addr2pubkey(addr,checkpub) == 0 || memcmp(pubkey,checkpub,32) != 0 )
        {
            printf("addr2pubkey %s error at %d\n",addr,i);
            return(-1);
        }
    }
    printf("addr2pubkey no errors\n");
    return(0);
}

void testrandom(char *origseed)
{
    int64_t stakeamount = 1;
    int32_t scheduledTick = 0;
    struct RevealAndCommit_input R;
    char rawhex[4096],txidstr[64],senderaddr[64];
    uint8_t pubkey[32],subseed[32],privkey[32],digest[32],entropy[512];
    memset(&R,0,sizeof(R));
    devurandom(entropy,sizeof(entropy));
    KangarooTwelve(entropy,sizeof(entropy),R.committedDigest,sizeof(R.committedDigest));
    getSubseedFromSeed((const uint8_t *)origseed,subseed);
    getPrivateKeyFromSubSeed(subseed,privkey);
    getPublicKeyFromPrivateKey(privkey,pubkey);
    getIdentityFromPublicKey(pubkey,senderaddr,false);
    while ( (scheduledTick= getlatest()) <= 1 )
    {
        printf("waiting for scheduledTick\n");
        sleep(1);
    }
    scheduledTick += 3;
    create_rawrevealcommit(rawhex,txidstr,digest,subseed,scheduledTick,&R,stakeamount);
    sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,rawhex);
    printf("scheduledTick.%d sizeof R %ld\n",scheduledTick,sizeof(R));
    while ( getlatest() != scheduledTick )
    {
        if ( (rand() % 200) == 0 )
            printf("waiting for scheduledTick.%d\n",scheduledTick);
        usleep(10000);
    }
    // get Tickdata and check for txid, if found:
    memcpy(R.bit4096,entropy,sizeof(R.bit4096));
    memset(R.committedDigest,0,sizeof(R.committedDigest));
    scheduledTick += 3;
    create_rawrevealcommit(rawhex,txidstr,digest,subseed,scheduledTick,&R,0);
    sendrawtransaction(randpeer(0),DEFAULT_NODE_PORT,rawhex);
    printf("scheduledTick.%d sizeof R %ld\n",scheduledTick,sizeof(R));
    while ( getlatest() != scheduledTick )
    {
        if ( (rand() % 200) == 0 )
            printf("waiting for scheduledTick.%d\n",scheduledTick);
        usleep(10000);
    }
    // get Tickdata and check for txid, if found display balance
}

struct mesg_buffer {
    long mesg_type;
    char mesg_text[100];
} message;
  
void msgtest(void)
{
    key_t key;
    int msgid;
    // ftok to generate unique key
    key = ftok("progfile", 65);
    // msgget creates a message queue
    // and returns identifier
    msgid = msgget(key, 0666 | IPC_CREAT);
    while ( 1 )
    {
        msgrcv(msgid, &message, sizeof(message), 1, 0);
        printf("Data Received is : %s \n",message.mesg_text);
    }
    // to destroy the message queue
    msgctl(msgid, IPC_RMID, NULL);
}

