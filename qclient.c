#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "qdefines.h"
#include "qstructs.h"
#include "K12AndKeyUtil.h"
#include "qkeys.c"
#include "qhelpers.c"


int32_t send_qbuffer(int32_t msgid,int64_t mtype,struct quheader *H,uint64_t respkey)
{
    int32_t hsz;
    struct qbuffer M;
    hsz = ((H->_size[2] << 16) + (H->_size[1] << 8) + H->_size[0]);
    M.mesg_type = mtype;
    memcpy(M.mesg_text,H,hsz);
    if ( respkey != 0 )
    {
        memcpy(&M.mesg_text[hsz],&respkey,sizeof(respkey));
        hsz += sizeof(respkey);
    }
    return(msgsnd(msgid,&M,hsz+sizeof(uint64_t),IPC_NOWAIT));
}

void *recvloop(void *_rcvmsgid)
{
    struct qbuffer R;
    int32_t sz,i,rcvmsgid = *(int32_t *)_rcvmsgid;
    printf("Start recvloop %d\n",rcvmsgid);
    while ( 1 )
    {
        if ( (sz= (int32_t)msgrcv(rcvmsgid,&R,sizeof(R),1,MSG_NOERROR)) > 0 )
        {
            /*for (i=0; i<sz; i++)
                printf("%02x",R.mesg_text[i]);
            printf(" %d bytes, type.%ld QRECV (%s)\n",sz,(long)R.mesg_type,(char *)R.mesg_text);*/
            printf("%s\n",(char *)R.mesg_text);
        }
    }
    return(0);
}

int main()
{
    pthread_t recv_thread;
    uint64_t key,rcvkey;
    uint8_t pubkey[32],data[2048];
    char line[4096];
    struct qbuffer M;
    struct quheader H;
    struct EntityRequest E;
    int msgid,rcvmsgid = 0,i,sz,len;
    setbuf(stdout, NULL);
    makefile(QUBIC_MSGPATH);
    key = ftok(QUBIC_MSGPATH, 'Q');
    msgid = msgget(key, 0666 | IPC_CREAT);
    //rcvkey = 0;
    //devurandom((uint8_t *)&rcvkey,7);
    //rcvmsgid = msgget(rcvkey, 0666 | IPC_CREAT);
    //printf("keys %lld %lld ids %d %d\n",(long long)key,(long long)rcvkey,msgid,rcvmsgid);
    //pthread_create(&recv_thread,NULL,&recvloop,(void *)&rcvmsgid);

    while ( 1 )
    {
        if ( fgets(line,sizeof(line)-1,stdin) != 0 )
        {
            len = strlen(line);
            if ( line[len-1] == '\n' )
                line[--len] = 0;
            if ( addr2pubkey(line,E.pubkey) > 0 )
            {
                E.H = quheaderset(REQUEST_ENTITY,sizeof(E));
                if ( rcvmsgid == 0 )
                {
                    rcvkey = 0;
                    memcpy(&rcvkey,E.pubkey,7);
                    rcvmsgid = msgget(rcvkey, 0666 | IPC_CREAT);
                    pthread_create(&recv_thread,NULL,&recvloop,(void *)&rcvmsgid);
                }
                if ( rcvmsgid != 0 )
                    send_qbuffer(msgid,2,&E.H,rcvkey);
            }
            else
            {
                //hexToByte(line,(uint8_t *)M.mesg_text,len/2);
                M.mesg_type = 1;
                H = quheaderset(0xff,len);
                memcpy(M.mesg_text,&H,sizeof(H));
                memcpy(&M.mesg_text[sizeof(H)],line,len+1);
                msgsnd(msgid,&M,len+1 + sizeof(H) + sizeof(uint64_t),IPC_NOWAIT);
            }
        }
    }
    return(0);
}
