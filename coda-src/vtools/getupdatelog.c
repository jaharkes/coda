#include <stdlib.h>
#include <string.h>

#include <lwp/lwp.h>
#include <rpc2/rpc2.h>
#include <rpc2/rpc2_addrinfo.h>

#include <vice.h>

int main(int argc, char **argv)
{
    RPC2_Options options;
    struct timeval tout = {
	.tv_sec = 15,
    };
    struct RPC2_addrinfo hints = {
	.ai_family = AF_INET,
	.ai_socktype = SOCK_DGRAM,
    };
    RPC2_HostIdent hident;
    RPC2_SubsysIdent sident;
    RPC2_BindParms bparms;
    RPC2_Handle conn;
    long rc;

#define LOG_MAX 128
    VolumeId volid;
    RPC2_Unsigned sequence, epoch, maxsequence;
    RPC2_Integer log_len;
    DirFid log[LOG_MAX];
    int i;

    if (argc < 2) {
	fprintf(stderr, "Usage: %s <volid>\n", argv[0]);
	exit(1);
    }

    rc = LWP_Init(LWP_VERSION, LWP_MAX_PRIORITY-1, NULL);
    if (rc != LWP_SUCCESS) {
	fprintf(stderr, "failed to initialize LWP\n");
	exit(1);
    }

    memset(&options, 0, sizeof(options));
    rc = RPC2_Init(RPC2_VERSION, &options, NULL, -1, &tout);
    if (rc != RPC2_SUCCESS) {
	fprintf(stderr, "failed to initialize RPC2: %s\n", RPC2_ErrorMsg(rc));
	exit(1);
    }

    hident.Tag = RPC2_HOSTBYADDRINFO;
    hident.Value.AddrInfo = NULL;
    rc = RPC2_getaddrinfo(NULL, "codasrv", &hints, &hident.Value.AddrInfo);
    if (rc) {
	fprintf(stderr, "RPC2_getaddrinfo failed with: %s\n",
		RPC2_gai_strerror(rc));
	exit(1);
    }

    sident.Tag = RPC2_SUBSYSBYID;
    sident.Value.SubsysId = SUBSYS_SRV;

    memset(&bparms, 0, sizeof(bparms));
    bparms.SecurityLevel = RPC2_OPENKIMONO;

    rc = RPC2_NewBinding(&hident, NULL, &sident, &bparms, &conn);
    if (rc != RPC2_SUCCESS)
    {
	fprintf(stderr, "RPC2_NewBinding failed with %s\n",
		RPC2_ErrorMsg((int)rc));
	exit(1);
    }

    volid = strtol(argv[1], NULL, 16);

    epoch = sequence = maxsequence = 0;
    log_len = 0;
    do {
	rc = ViceGetUpdateLog(conn, volid, &sequence, LOG_MAX, &log_len, log,
			      &epoch, &maxsequence);

	printf("ViceGetUpdateLog() = %ld\n\tsequence = %u\n\tlog_len = %d\n"
		"\tepoch = %u\n\tmaxsequence = %u\n",
		rc, sequence, log_len, epoch, maxsequence);
	for (i = 0; i < log_len; i++)
	    printf("\t\t(%x.%x)\n", log[i].Vnode, log[i].Unique);
    }
    while(sequence != maxsequence);

    RPC2_Unbind(conn);
    return 0;
}

