
struct Fanout
{
    char dests[FAN][FAN][FAN][64],txidsdir[512],origseed[64];
    int64_t total,depth1sum,depth2sum,depth3sum,sums1[FAN],sums2[FAN][FAN],sums3[FAN][FAN][FAN];
    int32_t numdests,depth,epochstart,latest;
};

void fanout_subseed(char *seed,char *iFAN,uint8_t subseed[32],char *addr)
{
    uint8_t privatekey[32],publickey[32];
    deriveaddr(seed,iFAN,subseed,privatekey,publickey,addr);
    //printf("(%s) -> %s, privkey: ",iFAN,addr);
    //for (int i=0; i<32; i++)
    //    printf("%02x",privatekey[i]);
    //printf("\n");
}

uint8_t Paymentpubkeys[FAN*FAN*FAN][32];
int64_t Startingbalances[FAN*FAN*FAN],Endingbalances[FAN*FAN*FAN],Paymentamounts[FAN*FAN*FAN];

int32_t calc_iFAN(char *iFAN,int32_t index,int32_t max)
{
    char rev[5];
    int32_t depth,i,j,n = max;
    if ( n >= FAN )
        n--;
    for (depth=0; depth<4&&n>0; depth++)
        n /= FAN;
    rev[0] = 'a' + (index % FAN);
    rev[1] = 'a' + ((index/FAN) % FAN);
    rev[2] = 'a' + ((index/(FAN*FAN)) % FAN);
    rev[3] = 'a' + ((index/(FAN*FAN*FAN)) % FAN);
    iFAN[depth] = 0;
    j = 0;
    for (i=depth-1; i>=0; i--)
        iFAN[i] = rev[j++];
    return(depth);
}

void fanout_addpubkey(struct pubkeypay *payments,int32_t index,char *dest,int64_t amount)
{
    addr2pubkey(dest,payments->publickeys[index]);
    payments->amounts[index] = amount;
}

void fanout_getdestaddr(int32_t numdests,char *origseed,char *addr,char *deststr,int64_t amount)
{
    char dest[64];
    uint8_t subseed[32];
    if ( deststr[0] != 0 )
    {
        strcpy(dest,deststr);
        if ( checkSumIdentity(dest) == 0 )
        {
            sprintf(dest,"dest.%s",deststr);
            fanout_subseed(origseed,dest,subseed,addr);
        }
        else
        {
            strcpy(addr,dest);
            dest[0] = 0;
        }
        if ( numdests < FAN*FAN )
            printf("[%s %s %s] ",dest,addr,amountstr(amount));
    }
    else
    {
        printf("unexpected null string at dest[][][]\n");
        exit(-1);
    }
}

void fanout_xyz(int32_t numdests,char *origseed,char *addr,int32_t destlimit,char *iFAN,char *deststr,int64_t amount)
{
    uint8_t subseed[32];
    if ( numdests <= destlimit )
        fanout_getdestaddr(numdests,origseed,addr,deststr,amount);
    else
    {
        fanout_subseed(origseed,iFAN,subseed,addr);
        printf("(%s %s %s) ",iFAN,addr,amountstr(amount));
    }
}

int64_t fanout_fees1(struct Fanout *fan,int32_t refi)
{
    int64_t fees = 0;
    int32_t slices,j;
    if ( fan->depth == 2 )
        return(SENDMANYFEE);
    else if ( fan->depth == 3 )
    {
        slices = fan->numdests / (FAN*FAN);
        if ( slices >= (refi + 1) )
            return(FAN*SENDMANYFEE + SENDMANYFEE);
        for (j=0; j<FAN; j++)
            if ( fan->sums2[refi][j] == 0 )
                break;
        return(j*SENDMANYFEE + SENDMANYFEE);
    }
    return(fees);
}

int64_t fanout_fees2(struct Fanout *fan,int32_t refi,int32_t refj)
{
    if ( fan->depth == 3 )
        return(SENDMANYFEE);
    else return(0);
}

int64_t sendZ_to_depth1(struct txq_item **waitforp,struct Fanout *fan,struct txq_item *waitfor)
{
    int32_t i,n = 0;
    int64_t amount,fees,sum = SENDMANYFEE;
    char dest[64],addr[64];
    uint8_t subseed[32];
    struct pubkeypay txq_vector;
    memset(&txq_vector,0,sizeof(txq_vector));
    fanout_subseed(fan->origseed,(char *)"Z",subseed,addr);
    printf("Z.%s -> ",addr);
    fan->total += SENDMANYFEE;
    for (i=0; i<FAN; i++)
    {
        if ( fan->sums1[i] != 0 )
        {
            fees = fanout_fees1(fan,i);
            amount = fan->sums1[i] + fees;
            sum += amount;
            fan->total += fees;
            sprintf(dest,"%c",'a' + i);
            fanout_xyz(fan->numdests,fan->origseed,addr,FAN,dest,fan->dests[0][0][i],amount);
            fanout_addpubkey(&txq_vector,n++,addr,amount);
        }
        else
            break;
    }
    printf(" payments.%d total %s\n",n,amountstr(fan->total));
    *waitforp = txq_queuetx(fan->txidsdir,subseed,&txq_vector,n,fan->epochstart,waitfor,0,1);
    return(sum);
}

int64_t send_depth1_to_depth2(struct txq_item *waitfors2[25],struct Fanout *fan,struct txq_item *waitfor)
{
    int32_t i,j,n;
    int64_t fees,amount,sum,total = SENDMANYFEE;
    char iFAN[6],dest[64],addr[64];
    uint8_t subseed[32];
    struct pubkeypay txq_vector;
    for (i=0; i<FAN; i++)
    {
        if ( fan->sums1[i] != 0 )
        {
            sprintf(iFAN,"%c",'a' + i);
            fanout_subseed(fan->origseed,iFAN,subseed,addr);
            printf("%s.%s -> ",iFAN,addr);
            n = 0;
            sum = SENDMANYFEE;
            memset(&txq_vector,0,sizeof(txq_vector));
            for (j=0; j<FAN; j++)
            {
                if ( fan->sums2[i][j] != 0 )
                {
                    fees = fanout_fees2(fan,i,j);
                    amount = fan->sums2[i][j] + fees;
                    sum += amount;
                    sprintf(dest,"%c%c",'a' + i,'a' + j);
                    fanout_xyz(fan->numdests,fan->origseed,addr,FAN*FAN,dest,fan->dests[0][i][j],amount);
                    fanout_addpubkey(&txq_vector,n++,addr,amount);
                }
            }
            printf(" sum %s\n",amountstr(sum));
            total += sum;
            waitfors2[i] = txq_queuetx(fan->txidsdir,subseed,&txq_vector,n,fan->epochstart,waitfor,(i * FAN),2);
        }
        else
            break;
    }
    return(total);
}

int64_t send_depth2_to_depth3(struct Fanout *fan,struct txq_item *waitfors2[25])
{
    int32_t i,j,k,n;
    int64_t sum = 0;
    char iFAN[6],dest[64],addr[64];
    uint8_t subseed[32];
    struct pubkeypay txq_vector;
    for (i=0; i<FAN; i++)
    {
        for (j=0; j<FAN; j++)
        {
            if ( fan->sums2[i][j] != 0 )
            {
                sprintf(iFAN,"%c%c",'a' + i,'a' + j);
                fanout_subseed(fan->origseed,iFAN,subseed,addr);
                //printf("%s.%s %s -> ",iFAN,addr,amountstr(fan->sums2[i][j]+SENDMANYFEE));
                n = 0;
                memset(&txq_vector,0,sizeof(txq_vector));
                for (k=0; k<FAN; k++)
                {
                    if ( fan->sums3[i][j][k] != 0 )
                    {
                        sum += fan->sums3[i][j][k];
                        sprintf(dest,"%c%c%c",'a' + i,'a' + j,'a' + k);
                        fanout_xyz(fan->numdests,fan->origseed,addr,FAN*FAN*FAN,dest,fan->dests[i][j][k],fan->sums3[i][j][k]);
                        fanout_addpubkey(&txq_vector,n++,addr,fan->sums3[i][j][k]);
                    }
                }
                //printf("\n");
                txq_queuetx(fan->txidsdir,subseed,&txq_vector,n,fan->epochstart,waitfors2[i],((i*FAN*FAN) + (j*FAN)),3);
            }
        }
    }
    return(sum);
}

void fanout_addpayment(struct Fanout *fan,int32_t i,char *destaddr,int64_t amount)
{
    char iFAN[5];
    memset(iFAN,0,sizeof(iFAN));
    calc_iFAN(iFAN,i,fan->numdests);
    if ( destaddr[0] == 0 )
        strcpy(destaddr,iFAN);
    fan->total += amount;
    fan->sums1[iFAN[0] - 'a'] += amount;
    if ( fan->numdests > FAN )
    {
        fan->sums2[iFAN[0] - 'a'][iFAN[1] - 'a'] += amount;
        if ( fan->numdests > FAN*FAN )
            fan->sums3[iFAN[0] - 'a'][iFAN[1] - 'a'][iFAN[2] - 'a'] += amount;
    }
    if ( fan->numdests <= FAN )
        strcpy(fan->dests[0][0][iFAN[0] - 'a'],destaddr);
    else if ( fan->numdests <= FAN*FAN )
        strcpy(fan->dests[0][iFAN[0] - 'a'][iFAN[1] - 'a'],destaddr);
    else if ( fan->numdests <= FAN*FAN*FAN )
        strcpy(fan->dests[iFAN[0] - 'a'][iFAN[1] - 'a'][iFAN[2] - 'a'],destaddr);
}

void fanout_send(struct Fanout *fan)
{
    struct txq_item *waitfor,*waitfors2[25];
    waitfor = 0;
    memset(waitfors2,0,sizeof(waitfors2));
    fan->depth1sum = sendZ_to_depth1(&waitfor,fan,0);
    if ( fan->numdests > FAN )
    {
        fan->depth2sum = send_depth1_to_depth2(waitfors2,fan,waitfor);
        if ( fan->numdests > FAN*FAN )
            fan->depth3sum = send_depth2_to_depth3(fan,waitfors2);
    }
}

struct Fanout *fanout_init(char *origseed,int32_t n,int32_t epochstart,char *txidsdir,int32_t latest)
{
    struct Fanout *fan;
    char iFAN[6],addr[64];
    uint8_t subseed[32];
    fan = (struct Fanout *)calloc(1,sizeof(*fan));
    fan->depth = calc_iFAN(iFAN,0,n);
    FANDEPTH = fan->depth;
    fan->numdests = n;
    fan->latest = latest;
    fan->epochstart = epochstart;
    strcpy(fan->txidsdir,txidsdir);
    strcpy(fan->origseed,origseed);
    fanout_subseed(origseed,(char *)"",subseed,addr);
    return(fan);
}

struct Fanout *fanout_create(char *origseed,char *fname,int32_t autogenflag)
{
    struct Fanout *fan = 0;
    uint8_t subseed[32],pubkey[32];
    int32_t i,j,n,tbepoch,latest,epochstart;
    char dest[64],line[512],txidsdir[1024],firstaddr[64],seedfname[2048];
    uint32_t seed;
    int64_t amount;
    FILE *fp;
    tbepoch = timebasedepoch(&epochstart,&latest);
    fanout_subseed(origseed,(char *)"Z",subseed,firstaddr);
    fanout_subseed(origseed,(char *)"",subseed,dest);
    makedir((char *)"sendmany");
    sprintf(txidsdir,"sendmany%c%s.%d",dir_delim(),dest,latest);
    makedir(txidsdir);
    printf("All data for this SENDMANY will be in %s\nSend required total to %s\n",txidsdir,firstaddr);
    if ( autogenflag != 0 )
    {
        sprintf(seedfname,"%s%cautogen.seed",txidsdir,dir_delim());
        if ( (fp= fopen(seedfname,"w")) != 0 )
        {
            fprintf(fp,"%s\n",origseed);
            fclose(fp);
        }
    }
    if ( fname == 0 || (n= atoi(fname)) == 0 )
    {
        devurandom((uint8_t *)&seed,sizeof(seed));
        srand(seed);
        while ( 1 )
        {
            n = (rand() % (FAN*FAN*FAN));
            if ( n > 10000 )
                break;
        }
        //n = 13758l;//12475;
    }
    if ( fname != 0 && fname[0] != 0 && (fp= fopen(fname,"r")) != 0 )
    {
        n = 0;
        for (i=0; i<2; i++)
        {
            rewind(fp);
            j = 0;
            while ( fgets(line,sizeof(line),fp) != 0 )
            {
                if ( line[strlen(line)-1] == '\n' )
                    line[strlen(line)-1] = 0;
                printf("(%s)\n",line);
                if ( i > 0 )
                {
                    memcpy(dest,line,60);
                    dest[60] = 0;
                    amount = atoll(line+61);
                    if ( line[60] != ',' || line[61] == 0 || amount <= 0 || checkSumIdentity(dest) == 0 || strcmp(dest,firstaddr) == 0 )
                    {
                        printf("line.%d ERROR (%s) must be valid address and more than 0 QU and not to Z address\n",j,dest);
                        free(fan);
                        fclose(fp);
                        return(0);
                    }
                    Paymentamounts[j] = amount;
                    fanout_addpayment(fan,j,dest,amount);
                    addr2pubkey(dest,pubkey);
                    memcpy(Paymentpubkeys[j],pubkey,sizeof(pubkey));
                    balancetickhash(pubkey,latest);
                }
                else n++;
                j++;
            }
            if ( i == 0 )
                fan = fanout_init(origseed,n,epochstart,txidsdir,latest);
        }
        fclose(fp);
        cpfile(fname,txidsdir);
    }
    else
    {
        char iFAN[6];
        if ( n <= 0 || n > FAN * FAN * FAN )
        {
            printf("illegal payments %d\n",n);
            exit(-3);
        }
        fan = fanout_init(origseed,n,epochstart,txidsdir,latest);
        for (i=0; i<n; i++)
        {
            dest[0] = 0;
            amount = 1;
            Paymentamounts[i] = amount;
            fanout_addpayment(fan,i,dest,amount);
            calc_iFAN(iFAN,i,fan->numdests);
            fanout_getdestaddr(fan->numdests,origseed,dest,iFAN,amount);
            addr2pubkey(dest,pubkey);
            memcpy(Paymentpubkeys[i],pubkey,sizeof(pubkey));
            balancetickhash(pubkey,latest);
        }
    }
    fprintf(stderr,"latest.%d init %d: ",latest,n);
    for (j=0; j<n; j++)
        balancetickhash(Paymentpubkeys[j],latest);
    for (j=0; j<n; j++)
    {
        balancetickhash(Paymentpubkeys[j],latest);
        if ( (j % 1000) == 0 )
        {
            fprintf(stderr,".");
            //sleep(1);
        }
        pubkey2addr(Paymentpubkeys[j],dest);
        //Startingbalances[j] = waitforbalance(0,Paymentpubkeys[j],latest);
        //printf("%s %s\n",dest,amountstr(Startingbalances[j]));
    }
    printf(" numpayments.%d total %s -> 1st address %s without fees\n",n,amountstr(fan->total),firstaddr);
    return(fan);
}
