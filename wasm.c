    


//#define TESTNET

#define MAX_INPUT_SIZE 1024
#ifdef TESTNET
#define DEFAULT_NODE_PORT 31841
#define DEFAULT_NODE_IP ((char *)"57.129.19.155") //193.135.9.63") //
#define TICKOFFSET 3
#else
#define DEFAULT_NODE_PORT 21841
#define DEFAULT_NODE_IP ((char *)"185.70.186.149")
#define TICKOFFSET 5
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/websocket.h>
#endif

#include "K12AndKeyUtil.h"

#include "qdefines.h"
#include "qstructs.h"
#include "json.c"
#include "qkeys.c"
#include "qhelpers.c"

#define JSON_BUFSIZE (65536)
#define MAX_INDEX 100
#define BALANCE_DEPTH 16

int32_t LATEST_TICK,VALIDATED_TICK,HAVE_TXTICK;

struct minientity
{
    uint8_t spectrum[32];
    int64_t totalin,totalout;
    int32_t tick;
};

struct balanceinfo
{
    char address[64];
    struct minientity fifo[BALANCE_DEPTH],valid;
};

struct balanceinfo Balances[MAX_INDEX];

struct pendingtx
{
    char address[64],dest[64],txid[64],password[512];
    int64_t amount;
    struct minientity before,after;
    int32_t pendingtick,pendingid,txreq,gottx,pwindex;
} PENDINGTX;

char PENDINGRESULT[4096],PENDINGSTATUS[4096];
char CURRENTRAWTX[MAX_INPUT_SIZE * 3];

// C code "linked" in by #include
#include "qtime.c"
#include "qtx.c"

char *wasm_result(int32_t retval,char *displaystr,int32_t seedpage)
{
    static char json[JSON_BUFSIZE],tmpstr[JSON_BUFSIZE-128];
    if ( displaystr[0] != '{' && displaystr[0] != '[' )
        sprintf(tmpstr,"\"%s\"",displaystr);
    else strcpy(tmpstr,displaystr);
    sprintf(json,"{\"result\":%d,\"display\":%s,\"seedpage\":%d}",retval,tmpstr,seedpage);
    return(json);
}

void accountfname(char *password,int32_t index,char *fname,uint8_t salt[32])
{
    char saltstr[65];
    KangarooTwelve((uint8_t *)password,(int32_t)strlen(password),salt,32);
    byteToHex(salt,saltstr,32);
    sprintf(fname,"%cqwallet%ckeys%c%s.%d",dir_delim(),dir_delim(),dir_delim(),saltstr+48,index);
}

int32_t accountcodec(char *rw,char *password,int32_t index,uint8_t subseed[32])
{
    FILE *fp;
    uint8_t salt[32];
    char fname[512];
    int32_t i,retval = -1;
    if ( index < 0 )
        return(-2);
    accountfname(password,index,fname,salt);
    //printf("check (%s) %s index.%d\n",fname,rw,index);
    if ( (fp= fopen(fname,rw)) != 0 )
    {
        //printf("opened (%s) %s\n",fname,rw);
        if ( strcmp(rw,"wb") == 0 )
        {
            for (i=0; i<sizeof(salt); i++)
                subseed[i] ^= salt[i];
            retval = fwrite(subseed,1,32,fp);
        }
        else
        {
            retval = fread(subseed,1,32,fp);
            for (i=0; i<sizeof(salt); i++)
                subseed[i] ^= salt[i];
        }
        fclose(fp);
    }
    //printf("%s -> retval %d\n",rw,retval);
    if ( retval == 32 )
        return(0);
    return(retval);
}

void subseedcombine(uint8_t subseed[32],uint8_t subseed2[32])
{
    uint8_t seedbuf[64];
    memcpy(seedbuf,subseed,32);
    memcpy(seedbuf+32,subseed2,32);
    KangarooTwelve(seedbuf,64,subseed,32);
    memset(seedbuf,0xff,sizeof(seedbuf));
}

char *_sendfunc(char **argv,int32_t argc,int32_t txtype)
{
    static char str[4096+1024];
    char *password,*dest,txid[64],addr[64],rawhex[4096];
    uint8_t txdigest[32],subseed[32],subseed2[32],privatekey[32],publickey[32],destpub[32],extradata[MAX_INPUT_SIZE];
    int64_t amount;
    int32_t txtick,datalen=0,i,pwindex = 0;
    if ( PENDINGTX.pendingid != 0 )
        return(wasm_result(-1,"Qwallet already has transaction pending",0));
    if ( argc < 5 || argc > 6 )
        return(wasm_result(-2,"_sendfunc needs password index txtick dest amount [hexstr]",0));
    password = argv[0];
    pwindex = atoi(argv[1]);
    if ( pwindex < 0 || pwindex >= MAX_INDEX )
        return(wasm_result(-3,"_sendfunc needs non negative index less than 100",0));
    txtick = atoi(argv[2]);
    dest = argv[3];
    if ( addr2pubkey(dest,destpub) == 0 )
    {
        char checkaddr[64];
        for (int i=0; dest[i]!=0; i++)
        {
            strcpy(checkaddr,dest);
            for (int j='A'; j<='Z'; j++)
            {
                checkaddr[i] = j;
                if ( checkSumIdentity(checkaddr) != 0 )
                {
                    sprintf(str,"_send illegal dest: changing %dth to %c to %s passes checksum",i,j,checkaddr);
                    return(wasm_result(-3,str,0));
                }
            }
        }
        return(wasm_result(-4,"illegal destination address, bad checksum",0));
    }
    amount = atoll(argv[4]);
    txid[0] = 0;
    if ( accountcodec("rb",password,0,subseed) == 0 )
    {
        if ( pwindex > 0 )
        {
            if ( accountcodec("rb",password,pwindex,subseed2) != 0 )
                return(wasm_result(-4,"cannot find derived key",0));
            subseedcombine(subseed,subseed2);
            memset(subseed2,0xff,sizeof(subseed2));
        }
        getPrivateKeyFromSubSeed(subseed,privatekey);
        getPublicKeyFromPrivateKey(privatekey,publickey);
        pubkey2addr(publickey,addr);
        if ( strcmp(addr,dest) == 0 )
            return(wasm_result(-4,"sending to same address not supported",0));
        if ( argc == 6 )
        {
            datalen = strlen(argv[5]) / 2;
            hexToByte(argv[5],extradata,datalen);
        }
        if ( txtick == 0 )
            txtick = LATEST_TICK + TICKOFFSET;
        create_rawtxhex(rawhex,txid,txdigest,subseed,txtype,publickey,destpub,amount,extradata,datalen,txtick);
        txid[60] = 0;
        printf("{\"txtick\":%d,\"txid\":\"%s\",\"rawhex\":\"%s\",\"addr\":\"%s\",\"amount\":%s,\"dest\":\"%s\"}",txtick,txid,rawhex,addr,amountstr(amount),dest);
        memset(subseed,0xff,sizeof(subseed));
        memset(privatekey,0xff,sizeof(privatekey));
        strcpy(CURRENTRAWTX,rawhex);
        for (i=0; i<MAX_INDEX; i++)
        {
            if ( strcmp(addr,Balances[i].address) == 0 )
            {
                PENDINGTX.pendingid = 1;
                strcpy(PENDINGTX.txid,txid);
                strcpy(PENDINGTX.password,password);
                PENDINGTX.pwindex = pwindex;
                strcpy(PENDINGTX.address,addr);
                strcpy(PENDINGTX.dest,dest);
                PENDINGTX.amount = amount;
                PENDINGTX.pendingtick = txtick;
                PENDINGTX.txreq = 0;
                PENDINGTX.gottx = 0;
                // check for valid balance and error if not enough funds
                PENDINGTX.before = Balances[i].valid;
                sprintf(PENDINGSTATUS,"transaction %s broadcast for tick %d",txid,txtick);
                break;
            }
        }
        memset(PENDINGRESULT,0,sizeof(PENDINGRESULT));
        if ( i == MAX_INDEX )
            return(wasm_result(0,"send broadcast but not queued since address could not be found",0));
        return(wasm_result(PENDINGTX.pendingid,"send queued",0));
    }
    return(wasm_result(-5,"unknown user account password file not found or invalid index",0));
}

char *sendfunc(char **argv,int32_t argc)
{
    return(_sendfunc(argv,argc,0));
}

char *sendmanyfunc(char **argv,int32_t argc)
{
    char hexstr[MAX_INPUT_SIZE*2+1],totalstr[16],destaddr[64];
    uint8_t destpub[32];
    int64_t total = 0;
    /*pubkeypay payments;
    memset(&payments,0,sizeof(payments));
    {
        //argv[3 + i*2], argv[4 + i*2]
        // put in pubkeys and amounts, += total;
    }
    byteToHex((uint8_t *)&payments,hexstr,sizeof(payments));*/
    memset(destpub,0,sizeof(destpub));
    ((uint64_t *)destpub)[0] = QUTIL_CONTRACT_ID;
    pubkey2addr(destpub,destaddr);
    total += SENDMANYFEE;
    sprintf(totalstr,"%s",amountstr(total));
    argc = 6;
    argv[3] = destaddr;
    argv[4] = totalstr;
    argv[5] = hexstr;
    return(_sendfunc(argv,argc,SENDTOMANYV1));
}

char *loginfunc(char **argv,int32_t argc)
{
    int32_t i,retval,index = 0;
    uint16_t bipi;
    uint64_t tmp;
    uint8_t privatekey[32],publickey[32],subseed[32],subseed2[32];
    char addr[64],seed[512],*password,bipwords[24][16];
    if ( argc == 0 || argc > 3 )
        return(wasm_result(-6,"login needs password",0));
    password = argv[0];
    devurandom(subseed,32);
    if ( argc >= 2 )
    {
        index = atoi(argv[1]);
        if ( index < 0 || index >= MAX_INDEX )
            return(wasm_result(-7,"login needs non negative index less than 100",0));
    }
    if ( accountcodec("rb",password,0,subseed) == 0 )
    {
        if ( index > 0 && index < MAX_INDEX )
        {
            if ( argc == 3 )
            {
                if ( accountcodec("rb",password,index,subseed2) == 0 )
                {
                    memset(subseed,0xff,sizeof(subseed));
                    memset(subseed2,0xff,sizeof(subseed2));
                    return(wasm_result(-8,"password already has derived subseed at index",0));
                }
                KangarooTwelve((uint8_t *)argv[2],strlen(argv[2]),subseed2,32);
                subseedcombine(subseed,subseed2);
                retval = accountcodec("wb",password,index,subseed2);
                memset(subseed2,0xff,sizeof(subseed2));
                if ( retval < 0 )
                {
                    memset(subseed,0xff,sizeof(subseed));
                    return(wasm_result(-9,"error creating encrypted derived index account",0));
                }
            }
            else
            {
                if ( accountcodec("rb",password,index,subseed2) != 0 )
                {
                    memset(subseed,0xff,sizeof(subseed));
                    return(wasm_result(-10,"password does not have derived subseed at index",0));
                }
                subseedcombine(subseed,subseed2);
                memset(subseed2,0xff,sizeof(subseed2));
            }
        }
        getPrivateKeyFromSubSeed(subseed,privatekey);
        getPublicKeyFromPrivateKey(privatekey,publickey);
        pubkey2addr(publickey,addr);
        //printf("found encrypted file for (%s) -> %s\n",password,addr);
        memset(subseed,0xff,sizeof(subseed));
        memset(privatekey,0xff,sizeof(privatekey));
        return(wasm_result(0,addr,0));
    }
    if ( index != 0 || argc > 2 )
        return(wasm_result(-11,"cannot create nonzero index or derivation without index.0",0));
  //printf("create encrypted file for %s\n",password);
    if ( argv[0][0] == 'Q' )
    {
        for (i=0; i<55; i++)
        {
            devurandom((uint8_t *)&tmp,sizeof(tmp));
            seed[i] = (tmp % 26) + 'a';
        }
        seed[55] = 0;
        getSubseedFromSeed((uint8_t *)seed,subseed);
    }
    else
    {
        seed[0] = 0;
        for (i=0; i<sizeof(bipwords)/sizeof(*bipwords); i++)
        {
            devurandom((uint8_t *)&bipi,sizeof(bipi));
            sprintf(seed+strlen(seed),"%s ",BIP39[bipi % (sizeof(BIP39)/sizeof(*BIP39))]);
        }
        seed[strlen(seed)-1] = 0;
        KangarooTwelve((uint8_t *)seed,strlen(seed),subseed,32);
    }
    getPrivateKeyFromSubSeed(subseed,privatekey);
    getPublicKeyFromPrivateKey(privatekey,publickey);
    memset(privatekey,0xff,sizeof(privatekey));
    retval = accountcodec("wb",password,index,subseed);
    memset(subseed,0xff,sizeof(subseed));
    if ( retval < 0 )
    {
        memset(seed,0xff,sizeof(seed));
        return(wasm_result(-12,"error creating encrypted account",0));
    }
    getIdentityFromPublicKey(publickey,addr,false);
    addr[60] = 0;
    printf("loginfunc got (%s) -> seed {%s} %s\n",password,seed,addr);
    return(wasm_result(retval,seed,1));
}

char *addseedfunc(char **argv,int32_t argc)
{
    int32_t i,retval=0;
    char *password,*seed;
    uint8_t privatekey[32],publickey[32],subseed[32];
    char addr[64],*origseed;
    if ( argc != 2 )
        return(wasm_result(-13,"addseed needs password seed",0));
    password = argv[0];
    seed = argv[1];
    if ( accountcodec("rb",password,0,subseed) == 0 )
        return(wasm_result(-14,"password already has seed",0));
    for (i=0; i<55; i++)
    {
        if ( seed[i] < 'a' || seed[i] > 'z' )
            break;
    }
    if ( i == 55 )
        getSubseedFromSeed((uint8_t *)seed,subseed);
    else KangarooTwelve((uint8_t *)seed,strlen(seed),subseed,32);
    getPrivateKeyFromSubSeed(subseed,privatekey);
    getPublicKeyFromPrivateKey(privatekey,publickey);
    memset(privatekey,0xff,sizeof(privatekey));
    retval = accountcodec("wb",password,0,subseed);
    memset(subseed,0xff,sizeof(subseed));
    if ( retval < 0 )
    {
        memset(seed,0xff,strlen(seed));
        return(wasm_result(-15,"error creating encrypted account",0));
    }
    getIdentityFromPublicKey(publickey,addr,false);
    addr[60] = 0;
    printf("addseed got (%s) -> seed {%s} %s\n",password,seed,addr);
    memset(seed,0xff,strlen(seed));
    return(wasm_result(retval,addr,0));
}

char *checkavailfunc(char **argv,int32_t argc)
{
    FILE *fp;
    uint8_t salt[32];
    char *password,fname[512];
    if ( argc != 1 )
        return(wasm_result(-20,"checkavail needs password",0));
    password = argv[0];
    accountfname(password,0,fname,salt);
    if ( (fp= fopen(fname,"rb")) == 0 )
        return(wasm_result(0,"password is available",0));
    fclose(fp);
    return(wasm_result(-33,"password already exists",0));
}

char *deletefunc(char **argv,int32_t argc)
{
    FILE *fp;
    uint8_t salt[32];
    int32_t index;
    char *password,fname[512],retstr[512];
    if ( argc != 2 )
        return(wasm_result(-20,"delete needs password,index",0));
    password = argv[0];
    index = atoi(argv[1]);
    accountfname(password,index,fname,salt);
    if ( (fp= fopen(fname,"rb")) == 0 )
        return(wasm_result(-21,"password,index has no file",0));
    fclose(fp);
    deletefile(fname);
    if ( index > 0 )
        return(wasm_result(0,"password,index file deleted",0));
    for (index=1; index<MAX_INDEX; index++)
    {
        accountfname(password,index,fname,salt);
        if ( (fp= fopen(fname,"rb")) != 0 )
        {
            fclose(fp);
            deletefile(fname);
        }
    }
    sprintf(retstr,"%d index files deleted for password",index);
    return(wasm_result(0,retstr,0));
}

char *listfunc(char **argv,int32_t argc)
{
    FILE *fp;
    int32_t index;
    uint8_t salt[32],origsubseed[32],subseed[32],subseed2[32],privkey[32],pubkey[32];
    char *password,fname[512],*retstr;
    static char addrs[MAX_INDEX][64],addrsarray[MAX_INDEX * 80];
    if ( argc != 1 )
        return(wasm_result(-20,"list needs password",0));
    password = argv[0];
    memset(addrs,0,sizeof(addrs));
    memset(Balances,0,sizeof(Balances));
    if ( accountcodec("rb",password,0,origsubseed) == 0 )
    {
        for (index=0; index<MAX_INDEX; index++)
        {
            memcpy(subseed,origsubseed,32);
            if ( index > 0 )
            {
                if ( accountcodec("rb",password,index,subseed2) != 0 )
                    continue;
                subseedcombine(subseed,subseed2);
            }
            getPrivateKeyFromSubSeed(subseed,privkey);
            getPublicKeyFromPrivateKey(privkey,pubkey);
            pubkey2addr(pubkey,addrs[index]);
            strcpy(Balances[index].address,addrs[index]);
            //printf("index.%d %s\n",index,addrs[index]);
        }
    }
    else return(wasm_result(-21,"password has no seed",0));
    memset(privkey,0xff,sizeof(privkey));
    memset(origsubseed,0xff,sizeof(origsubseed));
    memset(subseed,0xff,sizeof(subseed));
    memset(subseed2,0xff,sizeof(subseed2));
    strcpy(addrsarray,"[");
    for (index=0; index<MAX_INDEX; index++)
        sprintf(addrsarray + strlen(addrsarray),"\"%s\",",addrs[index]);
    addrsarray[strlen(addrsarray)-1] = ']';
    //printf("%s\n",addrsarray);
    retstr = wasm_result(0,addrsarray,0);
    printf("%s\n",retstr);
    return(retstr);
}

struct qcommands
{
    const char *command;
    char *(*func)(char **,int32_t);
    const char *helpstr;
} QCMDS[] =
{
    { "addseed", addseedfunc, "addseed password,seed" },
    { "login", loginfunc, "login password,[index [,derivation]]" },
    { "list", listfunc, "list password" },
    { "delete", deletefunc, "delete password,index" },
    { "checkavail", checkavailfunc, "checkavail password" },
    { "send", sendfunc, "send password,index,txtick,dest,amount[,extrahex]" },
    { "sendmany", sendmanyfunc, "send password,index,txtick,dest,amount[,dest2,amount2,...]" },
};

char *_qwallet(char *_args)
{
    int32_t i,j,len,argc = 0;
    char *argv[64],cmd[64],args[1024];
    args[sizeof(args)-1] = 0;
    strncpy(args,_args,sizeof(args)-1);
    for (i=0; args[i]!=0&&i<sizeof(cmd)-1; i++)
    {
        cmd[i] = args[i];
        if ( args[i] == ' ' )
            break;
    }
    cmd[i] = 0;
    //printf("args.(%s) -> cmd [%s]\n",args,cmd);
    for (i=0; i<sizeof(QCMDS)/sizeof(*QCMDS); i++)
    {
        if ( strcmp(cmd,QCMDS[i].command) == 0 )
        {
            len = (int32_t)strlen(cmd);
            while ( args[len] == ' ' || args[len] == '\t' || args[len] == '\r' || args[len] == '\n' )
                len++;
            argv[argc++] = &args[len];
            while ( args[len] != 0 )
            {
                if ( args[len] == ',' || args[len] == ';' || args[len] == '&' )
                {
                    args[len++] = 0;
                    argv[argc++] = &args[len];
                    if ( argc >= (sizeof(argv)/sizeof(*argv)) )
                        return(wasm_result(-5,"too many arguments",0));
                } else len++;
            }
            argv[argc] = (char *)"";
            //for (j=0; j<argc; j++)
            //    printf("{%s} ",argv[j]);
            //printf("argc.%d %s\n",argc,cmd);
            return((*QCMDS[i].func)(argv,argc));
        }
    }
    return(wasm_result(-1,"unknown command",0));
}

const char *json_strval(typed(json_element) element,char *field)
{
    result(json_element) command_element_result = json_object_find(element.value.as_object, field);
    if ( result_is_err(json_element)(&command_element_result) )
    {
        typed(json_error) error = result_unwrap_err(json_element)(&command_element_result);
        return("");
    }
    typed(json_element) command_element = result_unwrap(json_element)(&command_element_result);
    typed(json_element_value) value = command_element.value;
    return(value.as_string);
}

int64_t json_numval(typed(json_element) element,char *field)
{
    result(json_element) command_element_result = json_object_find(element.value.as_object, field);
    if ( result_is_err(json_element)(&command_element_result) )
    {
        typed(json_error) error = result_unwrap_err(json_element)(&command_element_result);
        return(0);
    }
    typed(json_element) command_element = result_unwrap(json_element)(&command_element_result);
    typed(json_element_value) value = command_element.value;
    return(value.as_number.value.as_long);
}

int32_t wssupdate(char *jsonstr)
{
    const char *command,*addr,*spectrum,*txid;
    int64_t input,output,sent;
    int32_t tick,index,f;
    uint8_t digest[32];
    result(json_element) element_result = json_parse(jsonstr);
    if ( result_is_err(json_element)(&element_result) )
    {
        typed(json_error) error = result_unwrap_err(json_element)(&element_result);
        fprintf(stderr, "Error parsing JSON: %s\n", json_error_to_string(error));
        return(-1);
    }
    typed(json_element) element = result_unwrap(json_element)(&element_result);
    command = json_strval(element,(char *)"command");
    if ( strcmp(command,(char *)"EntityInfo") == 0 )
    {
        addr = json_strval(element,(char *)"address");
        spectrum = json_strval(element,(char *)"spectrum");
        tick = json_numval(element,(char *)"tick");
        input = atoll(json_strval(element,(char *)"totalincoming"));
        output = atoll(json_strval(element,(char *)"totaloutgoing"));
        if ( tick != 0 )
        {
            for (index=0; index<MAX_INDEX; index++)
            {
                if ( strcmp(addr,Balances[index].address) == 0 )
                {
                    for (f=0; f<BALANCE_DEPTH; f++)
                    {
                        if ( Balances[index].fifo[f].tick == 0 )
                            break;
                    }
                    if ( f == BALANCE_DEPTH )
                    {
                        for (f=0; f<BALANCE_DEPTH-1; f++)
                            Balances[index].fifo[f] = Balances[index].fifo[f+1];
                    }
                    hexToByte(spectrum,Balances[index].fifo[f].spectrum,32);
                    Balances[index].fifo[f].tick = tick;
                    Balances[index].fifo[f].totalin = input;
                    Balances[index].fifo[f].totalout = output;
                    break;
                }
            }
        }
        printf("balance.(%s) index.%d tick.%d %s %s %s %s\n",addr,index,tick,amountstr3(input-output),amountstr(input),amountstr2(output),spectrum);
    }
    else if ( strcmp(command,(char *)"txidrequest") == 0 )
    {
        txid = json_strval(element,(char *)"txid");
        tick = json_numval(element,(char *)"tick");
        printf("JSON.(%s)\n",jsonstr);
        if ( PENDINGTX.pendingid != 0 && strcmp(PENDINGTX.txid,txid) == 0 && PENDINGTX.pendingtick == tick )
        {
            PENDINGTX.gottx = 1;
            sprintf(PENDINGSTATUS,"%s included in tick %d, waiting for balance change validation",PENDINGTX.txid,tick);
        }
        printf("txidrequest tick.%d %s txreq.%d gottx.%d\n",tick,txid,PENDINGTX.txreq,PENDINGTX.gottx);
    }
    else if ( strcmp(command,(char *)"validated") == 0 )
    {
        spectrum = json_strval(element,(char *)"spectrum");
        tick = json_numval(element,(char *)"tick");
        HAVE_TXTICK = json_numval(element,(char *)"havetx");
        hexToByte(spectrum,digest,32);
        VALIDATED_TICK = tick;
        //printf("validated tick.%d latest.%d %s\n",tick,LATEST_TICK,spectrum);
        for (index=0; index<MAX_INDEX; index++)
        {
            for (f=0; f<BALANCE_DEPTH; f++)
            {
                if ( Balances[index].fifo[f].tick == tick )
                {
                    char hexstr[65];
                    byteToHex(Balances[index].fifo[f].spectrum,hexstr,32);
                    //printf("%s Balanced[%d].fifo[%d].tick matches %d %s\n",Balances[index].address,index,f,Balances[index].fifo[f].tick,hexstr);
                    if ( memcmp(digest,Balances[index].fifo[f].spectrum,32) == 0 )
                    {
                        Balances[index].valid = Balances[index].fifo[f];
                        sent = 0;
                        if ( strcmp(PENDINGTX.address,Balances[index].address) == 0 && PENDINGTX.pendingid != 0 )
                        {
                            if ( tick < PENDINGTX.pendingtick )
                            {
                                printf("set %s pending.before from %d to %d, pendingtick.%d\n",PENDINGTX.address,PENDINGTX.before.tick,tick,PENDINGTX.pendingtick);
                                PENDINGTX.before = Balances[index].valid;
                            }
                            else if ( tick > PENDINGTX.pendingtick )
                            {
                                printf("tick %d > %d check for balance change sent %s vs %s\n",tick,PENDINGTX.pendingtick,amountstr(PENDINGTX.before.totalout),amountstr(Balances[index].valid.totalout));
                                if ( PENDINGTX.before.totalout != Balances[index].valid.totalout )
                                {
                                    sent = (Balances[index].valid.totalout - PENDINGTX.before.totalout);
                                    if ( sent == PENDINGTX.amount )
                                    {
                                        strcpy(PENDINGSTATUS,"send completed");
                                        sprintf(PENDINGRESULT,"{\"txtick\":%d,\"txid\":\"%s\",\"addr\":\"%s\",\"amount\":%s,\"dest\":\"%s\"}",PENDINGTX.pendingtick,PENDINGTX.txid,PENDINGTX.address,amountstr(PENDINGTX.amount),PENDINGTX.dest);
                                        memset(&PENDINGTX,0,sizeof(PENDINGTX));
                                    }
                                    else
                                    {
                                        sprintf(PENDINGSTATUS,"send %s error",PENDINGTX.txid);
                                        sprintf(PENDINGRESULT,"{\"error\":\"unexpected balance change %s instead of %s\"}",amountstr(sent),amountstr(PENDINGTX.amount));
                                        memset(&PENDINGTX,0,sizeof(PENDINGTX));
                                    }
                                }
                                else
                                {
                                    strcpy(PENDINGSTATUS,"pending send failed, resending");
                                    printf("PENDINGTX failed, resend\n");
                                    memset(PENDINGTX.txid,0,sizeof(PENDINGTX.txid));
                                    char *argv[6],numstr[16],numstr2[16];
                                    sprintf(numstr,"%d",PENDINGTX.pwindex);
                                    sprintf(numstr2,"%d",LATEST_TICK+TICKOFFSET);
                                    argv[0] = PENDINGTX.password;
                                    argv[1] = numstr;
                                    argv[2] = numstr2;
                                    argv[3] = PENDINGTX.dest;
                                    argv[4] = amountstr(PENDINGTX.amount);
                                    PENDINGTX.gottx = PENDINGTX.txreq = PENDINGTX.pendingtick = PENDINGTX.pendingid = 0;
                                    printf("resend %s\n",sendfunc(argv,5));
                                }
                            }
                        }
                        printf("spectrum match for %s.%d\n",Balances[index].address,tick);
                        break;
                    }
                    else printf("spectrum mismatch for %s.%d %s vs %s\n",Balances[index].address,tick,hexstr,spectrum);
                }
            }
        }
    }
    else if ( strcmp(command,(char *)"CurrentTickInfo") == 0 )
    {
        tick = json_numval(element,(char *)"tick");
        if ( tick > LATEST_TICK )
            LATEST_TICK = tick;
        //printf("current tick.%d latest.%d\n",tick,LATEST_TICK);
    }
    json_free(&element);
    return(0);
}

char *qwallet(char *_args)
{
    int32_t i,pendingid;
    char *retstr;
    static char retbuf[JSON_BUFSIZE],toggle;
    //if ( strcmp(_args,"v1request") != 0 )
    //    printf("qwallet(%s)\n",_args);
    if ( strcmp(_args,(char *)"help") == 0 )
    {
        retbuf[0] = 0;
        for (i=0; i<(sizeof(QCMDS)/sizeof(*QCMDS)); i++)
        {
            sprintf(retbuf+strlen(retbuf),"%s;",QCMDS[i].helpstr);
            printf("%s\n",QCMDS[i].helpstr);
        }
        return(wasm_result(0,retbuf,0));
    }
    else if ( strncmp(_args,(char *)"wss ",4) == 0 )
    {
        return(wasm_result(wssupdate(_args + 4),"got JSON",0));
    }
    else if ( strncmp(_args,(char *)"status",6) == 0 )
    {
        pendingid = atoi(_args+7);
        if ( PENDINGTX.pendingid == 0 )
            return(wasm_result(pendingid,"no command pending",0));
        if ( pendingid != 1 )
            return(wasm_result(-1,"invalid pendingid",0));
        if ( PENDINGRESULT[0] == 0 )
        {
            printf("%s\n",PENDINGSTATUS);
            return(wasm_result(pendingid,PENDINGSTATUS,0));
        }
        else
        {
            retstr = wasm_result(0,PENDINGRESULT,0);
            memset(PENDINGSTATUS,0,sizeof(PENDINGSTATUS));
            memset(PENDINGRESULT,0,sizeof(PENDINGRESULT));
            memset(&PENDINGTX,0,sizeof(PENDINGTX));
            return(retstr);
        }
    }
    else if ( strncmp(_args,(char *)"v1request",9) == 0 )
    {
        //printf("v1request %s\n",CURRENTRAWTX);
        if ( CURRENTRAWTX[0] != 0 )
        {
            retstr = wasm_result(0,CURRENTRAWTX,0);
            memset(CURRENTRAWTX,0,sizeof(CURRENTRAWTX));
            return(retstr);
        }
        else if ( PENDINGTX.pendingid != 0 && PENDINGTX.txreq == 0 && HAVE_TXTICK > PENDINGTX.pendingtick )
        {
            PENDINGTX.txreq = 1;
            sprintf(PENDINGSTATUS,"checking for %s tick.%d",PENDINGTX.txid,PENDINGTX.pendingtick);
            return(wasm_result(0,PENDINGTX.txid,0));
        }
        return(wasm_result(-1,"no request",0));
    }
    return(_qwallet(_args));
}

#ifdef EMSCRIPTEN

EM_JS(void, start_timer, (),
    {
        Module.timer = false;
        setTimeout(function() { Module.timer = true; }, 500);
    }
);
EM_JS(bool, check_timer, (), { return Module.timer; });


int32_t MAIN_count;

int main()
{
    int32_t i;
    printf("MAIN CALLED.%d\n",MAIN_count);
    MAIN_count++;
    MAIN_THREAD_EM_ASM(
           FS.mkdir('/qwallet');
           // FS.mount(IDBFS, {}, '/qwallet');
           FS.mount(NODEFS, { root: '.' }, '/qwallet');
           FS.syncfs(true, function (err) {
             assert(!err); });
    );
    //pthread_t mainloop_thread;
    //pthread_create(&mainloop_thread,NULL,&mainloop,0);
    start_timer();
    while ( 1 )
    {
        if ( check_timer() )
        {
            start_timer();
            MAIN_THREAD_EM_ASM(
                   FS.syncfs(function (err) {
                  assert(!err);
                });
            );
        }
        emscripten_sleep(100);
    }
}
#else
int main()
{
    makedir((char *)"qwallet");
    qwallet((char *)"login password");
    return(0);
}
#endif

// finish sendmany extradata construction

    
