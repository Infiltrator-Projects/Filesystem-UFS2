/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS-owned independent raw-image verifier. Migrated from Defragmenter's
 * Test Media. The caller supplies expected sizes and hashes; all on-disk
 * UFS2 parsing, cylinder-group walking, inode and indirect-block checks
 * execute inside UFS.so.
 */
#include "ifs_ufs_payload_verifier_v1.h"
#include "infiltratr/endian.h"
#include "infiltratr/posix_io.h"
#include <openssl/evp.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define LDTM_HASH_HEX 65u
static void ufs_digest_to_hex(const unsigned char *digest, unsigned int length,
                              char out[LDTM_HASH_HEX])
{
    static const char digits[] = "0123456789abcdef";
    if (length != 32u) { out[0] = 0; return; }
    for (unsigned i = 0; i < 32u; ++i) {
        out[2u*i] = digits[digest[i] >> 4];
        out[2u*i+1u] = digits[digest[i] & 15u];
    }
    out[64] = 0;
}

#define LDTM_UFS2_INODE_SIZE 256U
#define LDTM_UFS2_MAGIC_OFFSET 1372U
#define LDTM_UFS2_BSIZE_OFFSET 48U
#define LDTM_UFS2_FSIZE_OFFSET 52U
#define LDTM_UFS2_FRAG_OFFSET 56U
#define LDTM_UFS2_NCG_OFFSET 44U
#define LDTM_UFS2_CBLKNO_OFFSET 12U
#define LDTM_UFS2_IBLKNO_OFFSET 16U
#define LDTM_UFS2_CGSIZE_OFFSET 160U
#define LDTM_UFS2_IPG_OFFSET 184U
#define LDTM_UFS2_FPG_OFFSET 188U
#define LDTM_UFS2_INOPB_OFFSET 120U
#define LDTM_UFS2_CG_MAGIC UINT32_C(0x00090255)
#define LDTM_UFS2_CG_MAGIC_OFFSET 4U
#define LDTM_UFS2_CG_INDEX_OFFSET 12U
#define LDTM_UFS2_CG_IUSEDOFF_OFFSET 92U
#define LDTM_UFS2_IFMT UINT16_C(0170000)
#define LDTM_UFS2_IFREG UINT16_C(0100000)
#define LDTM_UFS2_NDADDR 12U

typedef struct {
    uint64_t superblock_offset;
    uint32_t block_size;
    uint32_t fragment_size;
    uint32_t fragments_per_block;
    uint32_t cylinder_groups;
    uint32_t cylinder_block_fragment;
    uint32_t inode_block_fragment;
    uint32_t cylinder_group_size;
    uint32_t inodes_per_group;
    uint32_t fragments_per_group;
    uint32_t inodes_per_block;
} LdtmUfs2Geometry;

static int ufs2_raw_read(int fd, uint64_t offset,
                         void *buffer, size_t length)
{
    return infiltratr_pread_full(fd, buffer, length, offset);
}

static int ufs2_raw_geometry(int fd, LdtmUfs2Geometry *geometry)
{
    static const uint64_t candidates[] = {
        UINT64_C(65536), UINT64_C(8192),
        UINT64_C(0), UINT64_C(262144)
    };
    uint8_t raw[8192];
    if (geometry == NULL)
        return -1;
    memset(geometry, 0, sizeof(*geometry));
    for (size_t candidate = 0U;
         candidate < sizeof(candidates) / sizeof(candidates[0]);
         ++candidate) {
        if (ufs2_raw_read(fd, candidates[candidate],
                          raw, sizeof(raw)) != 0)
            continue;
        if (raw[LDTM_UFS2_MAGIC_OFFSET] != 0x19U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 1U] != 0x01U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 2U] != 0x54U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 3U] != 0x19U)
            continue;
        geometry->superblock_offset = candidates[candidate];
        geometry->block_size =
            infiltratr_load_le32(raw + LDTM_UFS2_BSIZE_OFFSET);
        geometry->fragment_size =
            infiltratr_load_le32(raw + LDTM_UFS2_FSIZE_OFFSET);
        geometry->fragments_per_block =
            infiltratr_load_le32(raw + LDTM_UFS2_FRAG_OFFSET);
        geometry->cylinder_groups =
            infiltratr_load_le32(raw + LDTM_UFS2_NCG_OFFSET);
        geometry->cylinder_block_fragment =
            infiltratr_load_le32(raw + LDTM_UFS2_CBLKNO_OFFSET);
        geometry->inode_block_fragment =
            infiltratr_load_le32(raw + LDTM_UFS2_IBLKNO_OFFSET);
        geometry->cylinder_group_size =
            infiltratr_load_le32(raw + LDTM_UFS2_CGSIZE_OFFSET);
        geometry->inodes_per_group =
            infiltratr_load_le32(raw + LDTM_UFS2_IPG_OFFSET);
        geometry->fragments_per_group =
            infiltratr_load_le32(raw + LDTM_UFS2_FPG_OFFSET);
        geometry->inodes_per_block =
            infiltratr_load_le32(raw + LDTM_UFS2_INOPB_OFFSET);
        if (geometry->block_size < 4096U ||
            geometry->block_size > 65536U ||
            geometry->fragment_size < 512U ||
            geometry->fragment_size > geometry->block_size ||
            geometry->fragments_per_block == 0U ||
            geometry->fragment_size *
                geometry->fragments_per_block !=
                geometry->block_size ||
            geometry->cylinder_groups == 0U ||
            geometry->cylinder_group_size < 168U ||
            geometry->cylinder_group_size >
                geometry->block_size ||
            geometry->inodes_per_group == 0U ||
            geometry->fragments_per_group == 0U ||
            geometry->inodes_per_block !=
                geometry->block_size / LDTM_UFS2_INODE_SIZE)
            return -1;
        return 0;
    }
    return -1;
}

static int ufs2_raw_pointer(int fd,
                            const LdtmUfs2Geometry *geometry,
                            const uint8_t inode[LDTM_UFS2_INODE_SIZE],
                            uint64_t logical_block,
                            uint64_t *physical_fragment)
{
    const uint64_t nindir = geometry->block_size / 8U;
    uint64_t indices[3] = {0U, 0U, 0U};
    unsigned level = 0U;
    if (logical_block < LDTM_UFS2_NDADDR) {
        *physical_fragment =
            infiltratr_load_le64(
                inode + 112U + logical_block * 8U);
        return 0;
    }
    uint64_t remaining = logical_block - LDTM_UFS2_NDADDR;
    const uint64_t square = nindir * nindir;
    const uint64_t cube = square * nindir;
    if (remaining < nindir) {
        level = 1U;
        indices[0] = remaining;
    } else if ((remaining -= nindir) < square) {
        level = 2U;
        indices[0] = remaining / nindir;
        indices[1] = remaining % nindir;
    } else {
        remaining -= square;
        if (remaining >= cube)
            return -1;
        level = 3U;
        indices[0] = remaining / square;
        remaining %= square;
        indices[1] = remaining / nindir;
        indices[2] = remaining % nindir;
    }

    uint64_t current =
        infiltratr_load_le64(
            inode + 208U + (uint64_t)(level - 1U) * 8U);
    if (current == 0U) {
        *physical_fragment = 0U;
        return 0;
    }
    uint8_t entry[8];
    for (unsigned depth = 0U; depth < level; ++depth) {
        if (current % geometry->fragments_per_block != 0U)
            return -1;
        const uint64_t offset =
            current * geometry->fragment_size +
            indices[depth] * 8U;
        if (ufs2_raw_read(fd, offset, entry, sizeof(entry)) != 0)
            return -1;
        current = infiltratr_load_le64(entry);
        if (current == 0U && depth + 1U != level)
            return -1;
    }
    *physical_fragment = current;
    return 0;
}

static int ufs2_raw_hash_file(
    int fd, const LdtmUfs2Geometry *geometry,
    const uint8_t inode[LDTM_UFS2_INODE_SIZE],
    uint64_t size, char digest_hex[LDTM_HASH_HEX])
{
    EVP_MD_CTX *context = NULL;
    uint8_t *block = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    uint64_t remaining = size;
    uint64_t logical = 0U;
    int result = -1;

    context = EVP_MD_CTX_new();
    block = malloc(geometry->block_size);
    if (context == NULL || block == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1)
        goto cleanup;

    while (remaining > 0U) {
        uint64_t physical = 0U;
        if (ufs2_raw_pointer(
                fd, geometry, inode, logical,
                &physical) != 0 ||
            physical == 0U)
            goto cleanup;
        const size_t take =
            remaining < geometry->block_size
                ? (size_t)remaining
                : geometry->block_size;
        if (ufs2_raw_read(
                fd, physical * geometry->fragment_size,
                block, take) != 0 ||
            EVP_DigestUpdate(context, block, take) != 1)
            goto cleanup;
        remaining -= take;
        ++logical;
    }
    if (EVP_DigestFinal_ex(
            context, digest, &digest_length) != 1)
        goto cleanup;
    ufs_digest_to_hex(digest, digest_length, digest_hex);
    result = 0;

cleanup:
    free(block);
    if (context != NULL)
        EVP_MD_CTX_free(context);
    return result;
}

static int verify_ufs2_raw_payload(const char *path,
                                   const IfsUfsPayloadTarget *targets,
                                   size_t target_count,
                                   char *detail, size_t detail_capacity)
{
    LdtmUfs2Geometry geometry;
    uint8_t *cg = NULL;
    uint8_t inode[LDTM_UFS2_INODE_SIZE];
    uint8_t matched[IFS_UFS_PAYLOAD_VERIFIER_MAX_TARGETS] = {0};
    uint64_t regular_files = 0U;
    int fd = -1;
    int result = -1;

    if (detail != NULL && detail_capacity > 0U)
        detail[0] = '\0';
    if (path == NULL || !targets || !target_count ||
        target_count > IFS_UFS_PAYLOAD_VERIFIER_MAX_TARGETS)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ufs2_raw_geometry(fd, &geometry) != 0)
        goto cleanup;
    cg = malloc(geometry.cylinder_group_size);
    if (cg == NULL)
        goto cleanup;

    for (uint32_t group = 0U;
         group < geometry.cylinder_groups; ++group) {
        const uint64_t group_base =
            (uint64_t)group * geometry.fragments_per_group;
        const uint64_t cg_offset =
            (group_base +
             geometry.cylinder_block_fragment) *
            geometry.fragment_size;
        if (ufs2_raw_read(
                fd, cg_offset, cg,
                geometry.cylinder_group_size) != 0 ||
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_MAGIC_OFFSET) !=
                LDTM_UFS2_CG_MAGIC ||
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_INDEX_OFFSET) != group)
            goto cleanup;
        const uint32_t iused =
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_IUSEDOFF_OFFSET);
        const uint64_t map_bytes =
            ((uint64_t)geometry.inodes_per_group + 7U) / 8U;
        if (iused < 168U ||
            (uint64_t)iused + map_bytes >
                geometry.cylinder_group_size)
            goto cleanup;

        for (uint32_t local = 0U;
             local < geometry.inodes_per_group; ++local) {
            if ((cg[iused + (local >> 3U)] &
                 (uint8_t)(1U << (local & 7U))) == 0U)
                continue;
            const uint32_t inode_block =
                local / geometry.inodes_per_block;
            const uint32_t inode_slot =
                local % geometry.inodes_per_block;
            const uint64_t inode_fragment =
                group_base +
                geometry.inode_block_fragment +
                (uint64_t)inode_block *
                    geometry.fragments_per_block;
            const uint64_t inode_offset =
                inode_fragment * geometry.fragment_size +
                (uint64_t)inode_slot *
                    LDTM_UFS2_INODE_SIZE;
            if (ufs2_raw_read(
                    fd, inode_offset, inode,
                    sizeof(inode)) != 0)
                goto cleanup;
            const uint16_t mode =
                infiltratr_load_le16(inode);
            if ((mode & LDTM_UFS2_IFMT) !=
                LDTM_UFS2_IFREG)
                continue;
            const uint64_t size =
                infiltratr_load_le64(inode + 16U);
            ++regular_files;

            size_t candidate_count = 0U;
            for (size_t target = 0U;
                 target < target_count; ++target) {
                if (!matched[target] &&
                    targets[target].size_bytes == size)
                    candidate_count++;
            }
            if (candidate_count == 0U)
                continue;

            char hash[LDTM_HASH_HEX];
            if (ufs2_raw_hash_file(
                    fd, &geometry, inode, size, hash) != 0)
                goto cleanup;
            int found = 0;
            for (size_t target = 0U;
                 target < target_count; ++target) {
                if (!matched[target] &&
                    targets[target].size_bytes == size &&
                    strcmp(targets[target].sha256, hash) == 0) {
                    matched[target] = 1U;
                    found = 1;
                    break;
                }
            }
            if (!found)
                goto cleanup;
        }
    }

    for (size_t target = 0U;
         target < target_count; ++target) {
        if (!matched[target])
            goto cleanup;
    }
    if (regular_files != (uint64_t)target_count)
        goto cleanup;

    if (detail != NULL && detail_capacity > 0U)
        (void)snprintf(detail, detail_capacity,
                       "independent raw UFS2 verifier matched all %zu canonical payloads including "
                       "boundary-sized files",
                       target_count);
    result = 0;

cleanup:
    free(cg);
    if (fd >= 0)
        (void)close(fd);
    if (result != 0 && detail != NULL &&
        detail_capacity > 0U && detail[0] == '\0')
        (void)snprintf(
            detail, detail_capacity,
            "independent raw UFS2 payload verification failed");
    return result;
}

/* Versioned, independently discoverable interface. */
static const IfsUfsPayloadVerifierV1 payload_verifier = {
    IFS_UFS_PAYLOAD_VERIFIER_ABI_V1, sizeof(IfsUfsPayloadVerifierV1),
    "ufs2", verify_ufs2_raw_payload
};
const IfsUfsPayloadVerifierV1 *
infiltrator_filesystem_ufs_payload_verifier_v1(void)
{
    return &payload_verifier;
}
