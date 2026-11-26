/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_UFS2_MUTATION_H
#define IFS_UFS2_MUTATION_H
#include "ufs_native.h"
typedef struct Ufs2Mutation Ufs2Mutation;
typedef struct Ufs2RelocationPlan Ufs2RelocationPlan;
/* Writable access is qualified only for the clean native one-group profile.
 * The host grants the exclusive durable lease and retains it until close. */
int ufs2_mutation_open(const IfsFsHostRuntimeV2 *, const LdUfsSummary *, Ufs2Mutation **);
int ufs2_mutation_create(Ufs2Mutation *, const char *, uint32_t);
int ufs2_mutation_write(Ufs2Mutation *, const char *, uint64_t, const void *, uint32_t);
int ufs2_mutation_faulted(const Ufs2Mutation *);
void ufs2_mutation_counts(const Ufs2Mutation *, uint64_t *, uint64_t *);
void ufs2_mutation_close(Ufs2Mutation *);
int ufs2_relocation_plan(Ufs2Mutation *,const char *,uint32_t,Ufs2RelocationPlan **);
void ufs2_relocation_release(Ufs2Mutation *,Ufs2RelocationPlan *);
int ufs2_relocation_marker(const IfsFsHostRuntimeV2 *);
int ufs2_relocation_recover(const IfsFsHostRuntimeV2 *,int,uint64_t *,IfsFsVolumeInfoV2 *);
int ufs2_relocation_execute(Ufs2Mutation *,Ufs2RelocationPlan *,uint64_t *);
int ufs2_relocation_verify(Ufs2Mutation *,Ufs2RelocationPlan *);
#endif
