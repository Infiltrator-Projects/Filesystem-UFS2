/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
int ifs_ufs_worker_main(int argc, char **argv);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc, char **argv, const IfsDefragServicesV1 *services) {
    if (ifs_bind_services(services) != 0) return 1;
    const int result = ifs_ufs_worker_main(argc, argv);
    ifs_unbind_services();
    return result;
}
static const IfsUfsModuleV1 api = {
    IFS_UFS_MODULE_V1, sizeof(IfsUfsModuleV1),
    "ufs", IFS_UFS_CAP_RUN, run
};
const IfsUfsModuleV1 *infiltrator_filesystem_ufs_v1(void) { return &api; }
