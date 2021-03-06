/*
 * ynfs main function
 */


#include <sys/types.h>
#include <sys/wait.h>
#include <rpc/pmap_clnt.h>
#include <errno.h>
#include <getopt.h>

#define DBG_SUBSYS S_YNFS

#include "get_version.h"
#include "nfs_events.h"
#include "nfs_state_machine.h"
#include "ylib.h"
#include "sunrpc_passive.h"
#include "sunrpc_proto.h"
#include "sdfs_lib.h"
#include "xdr_nfs.h"
#include "ynet_rpc.h"
#include "ynfs_conf.h"
#include "md_lib.h"
#include "configure.h"
#include "nfs_proc.h"
#include "net_table.h"
#include "network.h"
#include "proc.h"
#include "sdfs_quota.h"
#include "core.h"
#include "nlm_async.h"
#include "io_analysis.h"
#include "allocator.h"
#include "dbg.h"

static int nfs_srv_running;
extern char *__restart_cmd__;
nfs_analysis_t nfs_analysis;
int nfs_remove_init();

/*
extern int fnotify_register(const char *path, fnotify_callback mod_callback,
                     fnotify_callback del_callback, void *context);
extern int fnotify_init(void);
*/

typedef struct {
        int daemon;
        char home[MAX_PATH_LEN];
} ynfs_args_t;

static void nfs_signal_handler(int sig)
{
        (void) sig;
        //DINFO("got signal %d, load %llu\n", sig, (LLU)jobdock_load());

        if (nfs_srv_running == 0)
                return;
        
        DINFO("got signal %d\n", sig);
        //inode_proto_dump();
        netable_iterate();
        analysis_dump();
}

static void nfs_exit_handler(int sig)
{
        DINFO("got signal %d, exiting\n", sig);

        nfs_srv_running = 0;
        srv_running = 0;
}

/* register with portmapper ? */
int opt_portmapper = 1;
int nfs_epollfd = -1;
extern char *nid_path;

int register_nfs_service()
{
        int ret;

        if (opt_portmapper)
                pmap_unset(NFS3_PROGRAM, NFS_V3);

        if (! pmap_set (NFS3_PROGRAM, NFS_V3, opt_portmapper ? IPPROTO_TCP : 0,
                        2049)) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int register_mount_service()
{
        int ret;

        if (opt_portmapper) {
                pmap_unset(MOUNTPROG, MOUNTVERS3);
        }

        if (! pmap_set (MOUNTPROG, MOUNTVERS3, opt_portmapper ? IPPROTO_TCP : 0,
                        2049)) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return 0;
}

int register_nlm_service()
{
        int ret;

        if (opt_portmapper)
                pmap_unset(NLM_PROGRAM, NLM_VERSION);

        if (! pmap_set (NLM_PROGRAM, NLM_VERSION, opt_portmapper ? IPPROTO_TCP : 0,
                        3001)) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }
        return 0;
err_ret:
        return ret;
}

int ynfs_reset_handler(net_handle_t *nh, uuid_t *nodeid)
{
        (void) nodeid;
        net_handle_t *mds_nid;

        mds_nid = &ng.mds_nh;

        if (nid_cmp(&nh->u.nid, &mds_nid->u.nid) == 0) {
                DWARN("mds off\n");
        } else
                DWARN("peer off\n");

        return 0;
}

int ynfs_srv(void *args)
{
        int ret, daemon;
        net_proto_t net_op;
        ynfs_args_t *ynfs_args;
        const char *service = NFS_SERVICE_DEF;
        char path[MAX_PATH_LEN];

        ynfs_args = args;
        daemon = ynfs_args->daemon;

        snprintf(path, MAX_NAME_LEN, "%s/status/status.pid", ynfs_args->home);
        ret = daemon_pid(path);
        if (ret)
                GOTO(err_ret, ret);

        ret = mountlist_init();
        if (ret)
                GOTO(err_ret, ret);

        /* init write verifier */
        regenerate_write_verifier();

        memset(&net_op, 0x0, sizeof(net_proto_t));

        //net_op.reset_handler = ynfs_reset_handler;

        ret = sy_spin_init(&nfs_conf_lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ly_init(daemon, "nfs/0", -1);
        if (ret)
                GOTO(err_ret, ret);

        ret = io_analysis_init("nfs", 0);
        if (ret)
                GOTO(err_ret, ret);

        ret = allocator_init();
        if (ret)
                GOTO(err_ret, ret);
        
        ret = core_init(0);
        if (ret)
                GOTO(err_ret, ret);
        
retry:
        ret = network_connect_master();
        if (ret) {
                ret = _errno(ret);
                if (ret == EAGAIN) {
                        sleep(5);
                        goto retry;
                } else
                        GOTO(err_ret, ret);
        }

        ret = nfs_analysis_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = nfs_remove_init();
        if (ret)
                GOTO(err_ret, ret);

        UNIMPLEMENTED(__WARN__);
#if 0
        DINFO("step 2_1\n");
        ret = __nfs_conf_register(__conf_modified, __conf_delete, NULL);
        if(ret)
                GOTO(err_ret, ret);

#if PROC_MONITOR_ON
        ret = proc_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = proc_log("ynfs");
        if (ret)
                GOTO(err_ret, ret);
#endif

        pthread_t th_cdsstate;
        pthread_attr_t ta;
        (void) pthread_attr_init(&ta);
        (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);

        ret = pthread_create(&th_cdsstate, &ta, handler_ynfsstate, NULL);
#endif

        DINFO("nfs started...\n");

        ret = rpc_start(); /*begin serivce*/
        if (ret)
                GOTO(err_ret, ret);

        ret = sunrpc_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = sunrpc_tcp_passive(service);
        if (ret)
                GOTO(err_ret, ret);

        ret = register_mount_service();
        if (ret) {
                DERROR("you need portmap installed...\n");
                GOTO(err_ret, ret);
        }

        ret = register_nfs_service();
        if (ret) {
                DERROR("you need portmap installed...\n");
                GOTO(err_ret, ret);
        }

#if 0
        ret = sunrpc_tcp_passive(NLM_SERVICE_DEF);
        if (ret)
                GOTO(err_ret, ret);

        ret = nlm4_async_init();
        if (ret)
                GOTO(err_ret, ret);
        
        ret = register_nlm_service();
        if (ret) {
                DERROR("you need portmap installed...\n");
                GOTO(err_ret, ret);
        }
#endif

        ret = ly_update_status("running", -1);
        if (ret)
                GOTO(err_ret, ret);

        nfs_srv_running = 1;

        DINFO("begin running\n");
        
        while (nfs_srv_running) { //we got nothing to do here
                sleep(10);
        }

        ret = ly_update_status("stopping", -1);
        if (ret)
                GOTO(err_ret, ret);

        DINFO("exiting...\n");

        ret = ly_update_status("stopped", -1);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int main(int argc, char *argv[])
{
        int ret, daemon = 1, maxcore __attribute__((unused)) = 0;
        int c_opt;
        ynfs_args_t ynfs_args;
        char name[MAX_NAME_LEN], *home;

        while (srv_running) {
                int option_index = 0;

#if 0
                static struct option long_options[] = {
                        { "init", no_argument, 0, 0},
                        { "clustername", required_argument, 0, 0},
                        { "foreground", no_argument, 0, 'f' },
                        { "help", no_argument, 0, 'h' },
                        { 0, 0, 0, 0 },
                };
#endif

#if 1
                static struct option long_options[] = {
                        {"spliceoff", 0, 0, 0},
                        {"sparseoff", 0, 0, 0},
                        { "home", required_argument, 0, 'h'},
                };
#endif

                c_opt = getopt_long(argc, argv, "cfm:h:v",
                                    long_options, &option_index);
                if (c_opt == -1)
                        break;

                switch (c_opt) {
#if 1
                case 0:
                        switch (option_index) {
                        case 0:
                                break;
                        case 1:
                                break;
                        default:
                                fprintf(stderr, "Hoops, wrong op got!\n");
                        }

                        break;
#endif
                case 'c':
                        maxcore = 1;
                        break;
                case 'f':
                        daemon = 2;
                        break;
                case 'v':
                        get_version();
                        exit(0);
                case 'h':
                        home = optarg;
                        break;
                default:
                        fprintf(stderr, "Hoops, wrong op (%c) got!\n", c_opt);
                        exit(1);
                }
        }

        if (home == NULL) {
                fprintf(stderr, "set --home <dir> please\n");
                exit(1);
        }

        strcpy(ynfs_args.home, home);
        strcpy(ng.home, home);
        
        //__restart_cmd__ = argv[0];
        //
        strcpy(name, "nfs/0");
        ret = ly_prep(daemon, name, -1);
        if (ret)
                GOTO(err_ret, ret);

        if (strcmp(gloconf.nfs, "native")) {
                DWARN("native nfs disabled, use %s replace\n", gloconf.nfs);
                exit(1);
        }
        
        ynfs_args.daemon = daemon;

        signal(SIGUSR1, nfs_signal_handler);
        signal(SIGUSR2, nfs_exit_handler);
        signal(SIGTERM, nfs_exit_handler);
        signal(SIGHUP, nfs_exit_handler);
        signal(SIGKILL, nfs_exit_handler);
        signal(SIGINT, nfs_exit_handler);

        ret = ly_run(home, ynfs_srv, &ynfs_args);
        if (ret)
                GOTO(err_ret, ret);

        (void) ylog_destroy();

        return 0;
err_ret:
        return ret;
}
