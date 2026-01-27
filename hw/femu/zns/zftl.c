#include "zns.h"

//#define FEMU_DEBUG_ZFTL

static void *ftl_thread(void *arg);

static inline struct ppa get_maptbl_ent(struct zns_ssd *zns, uint64_t lpn)
{
    return zns->maptbl[lpn];
}

static inline void set_maptbl_ent(struct zns_ssd *zns, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < zns->l2p_sz);
    zns->maptbl[lpn] = *ppa;
}

void zftl_init(FemuCtrl *n)
{
    struct zns_ssd *ssd = n->zns;

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline struct zns_ch *get_ch(struct zns_ssd *zns, struct ppa *ppa)
{
    return &(zns->ch[ppa->g.ch]);
}

static inline struct zns_fc *get_fc(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_ch *ch = get_ch(zns, ppa);
    return &(ch->fc[ppa->g.fc]);
}

static inline struct zns_plane *get_plane(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_fc *fc = get_fc(zns, ppa);
    return &(fc->plane[ppa->g.pl]);
}

static inline struct zns_blk *get_blk(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_plane *pl = get_plane(zns, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline void check_addr(int a, int max)
{
   assert(a >= 0 && a < max);
}

static void zns_advance_write_pointer(struct zns_ssd *zns)
{
    struct write_pointer *wpp = &zns->wp;

    check_addr(wpp->ch, zns->num_ch);
    wpp->ch++;
    if (wpp->ch == zns->num_ch) {
        wpp->ch = 0;
        check_addr(wpp->lun, zns->num_lun);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == zns->num_lun) {
            wpp->lun = 0;
        }
    }
}




static uint64_t zns_advance_status(struct zns_ssd *zns, struct ppa *ppa,struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;

    uint64_t nand_stime;
    uint64_t req_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;

    //plane level parallism
    struct zns_plane *pl = get_plane(zns, ppa);

    uint64_t lat = 0;
    int nand_type = get_blk(zns,ppa)->nand_type;

    uint64_t read_delay = zns->timing.pg_rd_lat[nand_type];
    uint64_t write_delay = zns->timing.pg_wr_lat[nand_type];
    uint64_t erase_delay = zns->timing.blk_er_lat[nand_type];

    switch (c) {
    case NAND_READ:
        nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
                     pl->next_plane_avail_time;
        pl->next_plane_avail_time = nand_stime + read_delay;
        lat = pl->next_plane_avail_time - req_stime;
	    break;

    case NAND_WRITE:
	    nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
		            pl->next_plane_avail_time;
	    pl->next_plane_avail_time = nand_stime + write_delay;
	    lat = pl->next_plane_avail_time - req_stime;
	    break;

    case NAND_ERASE:
        nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
                        pl->next_plane_avail_time;
        pl->next_plane_avail_time = nand_stime + erase_delay;
        lat = pl->next_plane_avail_time - req_stime;
        break;

    default:
        /* To silent warnings */
        ;
    }

    return lat;
}

static inline bool valid_ppa(struct zns_ssd *zns, struct ppa *ppa)
{
    int ch = ppa->g.ch;
    int lun = ppa->g.fc;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sub_pg = ppa->g.spg;

    if (ch >= 0 && ch < zns->num_ch && lun >= 0 && lun < zns->num_lun && pl >=
        0 && pl < zns->num_plane && blk >= 0 && blk < zns->num_blk && pg>=0 && pg < zns->num_page && sub_pg >= 0 && sub_pg < ZNS_PAGE_SIZE/LOGICAL_PAGE_SIZE)
        return true;

    return false;
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static struct ppa get_new_page(struct zns_ssd *zns)
{
    struct write_pointer *wpp = &zns->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.fc = wpp->lun;
    ppa.g.blk = zns->active_zone;
    ppa.g.V = 1; //not padding page
    if(!valid_ppa(zns,&ppa))
    {
        ftl_err("[Misao] invalid ppa: ch %u lun %u pl %u blk %u pg %u subpg  %u \n",ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg);
        ppa.ppa = UNMAPPED_PPA;
    }
    return ppa;
}

static int zns_get_wcidx(struct zns_ssd* zns)
{
    int i;
    for(i = 0;i < zns->cache.num_wc;i++)
    {
        if(zns->cache.write_cache[i].sblk==zns->active_zone)
        {
            return i;
        }
    }
    return -1;
}

static void zns_invalidate_zone_cache(struct zns_ssd *zns, uint32_t zone_idx)
{
    int i;

    for (i = 0; i < zns->cache.num_wc; i++) {
        if (zns->cache.write_cache[i].sblk == zone_idx) {
            #ifdef FEMU_DEBUG_ZFTL
            uint64_t discarded_entries = zns->cache.write_cache[i].used;
            #endif

            zns->cache.write_cache[i].used = 0;
            zns->cache.write_cache[i].sblk = INVALID_SBLK;

            ftl_debug("Invalidated write_cache[%d] for zone %u (%lu entries discarded)\n",
                     i, zone_idx, discarded_entries);
            return;
        }
    }
}

static void zns_invalidate_zone_mappings(struct zns_ssd *zns, uint32_t zone_idx,
                                         uint64_t zone_size_lbas, uint64_t lbasz)
{
    uint64_t zone_start_lba = zone_idx * zone_size_lbas;
    uint64_t zone_end_lba = (zone_idx + 1) * zone_size_lbas;
    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE / lbasz;
    uint64_t start_lpn = zone_start_lba / secs_per_pg;
    uint64_t end_lpn = zone_end_lba / secs_per_pg;
    uint64_t lpn;
    uint64_t invalidated_count = 0;

    for (lpn = start_lpn; lpn < end_lpn && lpn < zns->l2p_sz; lpn++) {
        if (zns->maptbl[lpn].ppa != UNMAPPED_PPA) {
            zns->maptbl[lpn].ppa = UNMAPPED_PPA;
            invalidated_count++;
        }
    }

    ftl_debug("Invalidated %lu LPN mappings for zone %u (LPN %lu-%lu)\n",
             invalidated_count, zone_idx, start_lpn, end_lpn - 1);
}

static void zns_reset_block_state(struct zns_ssd *zns, uint32_t zone_idx)
{
    int ch, lun, pl;
    struct ppa ppa;
    struct zns_blk *blk;

    for (ch = 0; ch < zns->num_ch; ch++) {
        for (lun = 0; lun < zns->num_lun; lun++) {
            for (pl = 0; pl < zns->num_plane; pl++) {
                ppa.g.ch = ch;
                ppa.g.fc = lun;
                ppa.g.pl = pl;
                ppa.g.blk = zone_idx;
                ppa.g.pg = 0;
                ppa.g.spg = 0;

                blk = get_blk(zns, &ppa);
                blk->page_wp = 0;
            }
        }
    }

    ftl_debug("Reset block state for zone %u (all page_wp = 0)\n", zone_idx);
}

uint64_t zns_zone_reset(struct zns_ssd *zns, uint32_t zone_idx,
                        uint64_t zone_size_lbas, uint64_t lbasz, uint64_t stime)
{
    int ch, lun, pl;
    struct ppa ppa;
    struct nand_cmd erase_cmd;
    uint64_t sublat, maxlat = 0;
    uint64_t total_blocks_erased = 0;

    ftl_debug("=== Zone Reset Started for Zone %u ===\n", zone_idx);

    /* Step 1: Invalidate write cache (instant) */
    zns_invalidate_zone_cache(zns, zone_idx);

    /* Step 2: Invalidate L2P mappings (instant) */
    zns_invalidate_zone_mappings(zns, zone_idx, zone_size_lbas, lbasz);

    /* Step 3: Reset block state (instant) */
    zns_reset_block_state(zns, zone_idx);

    /* Step 4: Simulate physical erase across all channels/LUNs/planes (parallel) */
    erase_cmd.type = USER_IO;
    erase_cmd.cmd = NAND_ERASE;
    erase_cmd.stime = stime;

    for (ch = 0; ch < zns->num_ch; ch++) {
        for (lun = 0; lun < zns->num_lun; lun++) {
            for (pl = 0; pl < zns->num_plane; pl++) {
                ppa.ppa = 0;
                ppa.g.ch = ch;
                ppa.g.fc = lun;
                ppa.g.pl = pl;
                ppa.g.blk = zone_idx;
                ppa.g.pg = 0;
                ppa.g.spg = 0;

                sublat = zns_advance_status(zns, &ppa, &erase_cmd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
                total_blocks_erased++;
            }
        }
    }

    ftl_debug("Zone %u reset complete: erased %lu blocks across %d ch * %d lun * %d planes\n",
             zone_idx, total_blocks_erased, (int)zns->num_ch, (int)zns->num_lun, (int)zns->num_plane);
    ftl_debug("Maximum erase latency: %lu ns (%.2f ms)\n", maxlat, maxlat / 1000000.0);
    ftl_debug("=== Zone Reset Finished ===\n\n");

    return maxlat;
}

static uint64_t zns_read(FemuCtrl *n, struct zns_ssd *zns, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    uint32_t nlb = req->nlb;
    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE/zns->lbasz;
    uint64_t start_lpn = lba / secs_per_pg;
    uint64_t end_lpn = (lba + nlb - 1) / secs_per_pg;
    //int wcidx = zns_get_wcidx(zns);
    struct ppa ppa;
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0;
    uint64_t ezrwa = 0;
    NvmeNamespace *ns = req->ns;
    NvmeZone *zone = zns_get_zone_by_slba(ns, lba);

    if (zone->d.za & NVME_ZA_ZRWA_VALID){
        ezrwa = zone->w_ptr + n->zns->zrwas - 1;
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        if ((zone->d.za & NVME_ZA_ZRWA_VALID) && lpn * secs_per_pg < ezrwa) {
            sublat += SRAM_WRITE_LATENCY_NS;
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }else{
            ppa = get_maptbl_ent(zns, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(zns, &ppa)) {
                continue;
            }

            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;

            sublat = zns_advance_status(zns, &ppa, &srd);
            ftl_debug("[R] lpn:\t%lu\t<--ch:\t%u\tlun:\t%u\tpl:\t%u\tblk:\t%u\tpg:\t%u\tsubpg:\t%u\tlat\t%lu\n",lpn,ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg,sublat);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
    }

    return maxlat;
}

static uint64_t zns_wc_flush(struct zns_ssd* zns, int wcidx, int type, uint64_t stime)
{
    int i,subpage;
    struct ppa ppa;
    struct ppa oldppa;
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0;

    i = 0;
    while(i < zns->cache.write_cache[wcidx].used){

        /* new write */
        ppa = get_new_page(zns);
        ppa.g.pl = 0;
        ppa.g.pg = get_blk(zns, &ppa)->page_wp;
        get_blk(zns, &ppa)->page_wp++;
        for (subpage = 0; subpage < ZNS_PAGE_SIZE / LOGICAL_PAGE_SIZE; subpage++)
        {
            if (i + subpage >= zns->cache.write_cache[wcidx].used)
            {
                //No need to write an invalid page
                break;
            }
            lpn = zns->cache.write_cache[wcidx].lpns[i + subpage];
            oldppa = get_maptbl_ent(zns, lpn);
            if (mapped_ppa(&oldppa)) {
                /* FIXME: Misao: update old page information*/
            }
            ppa.g.spg = subpage;
            /* update maptbl */
            set_maptbl_ent(zns, lpn, &ppa);
            // ftl_debug("[F] lpn:\t%lu\t-->ch:\t%u\tlun:\t%u\tpl:\t%u\tblk:\t%u\tpg:\t%u\tsubpg:\t%u\tlat\t%lu\n",lpn,ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg,sublat);
        }
        i += ZNS_PAGE_SIZE / LOGICAL_PAGE_SIZE;
        
        //FIXME Misao: identify padding page
        if(ppa.g.V)
        {
            struct nand_cmd swr;
            swr.type = type;
            swr.cmd = NAND_WRITE;
            swr.stime = stime;
            /* get latency statistics */
            sublat = zns_advance_status(zns, &ppa, &swr);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
        
        /* need to advance the write pointer here */
        zns_advance_write_pointer(zns);
    }
    zns->cache.write_cache[wcidx].used = 0;
    return maxlat;
}

static uint64_t zns_write(FemuCtrl *n, struct zns_ssd *zns, NvmeRequest *req)
{
    struct ppa ppa, oldppa;
    int subpage = 0;
    uint64_t lba = req->slba;
    uint64_t plba = 0;
    uint32_t nlb = req->nlb;
    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE / zns->lbasz;
    uint64_t start_lpn = lba / secs_per_pg;
    uint64_t end_lpn = (lba + nlb - 1) / secs_per_pg;
    uint64_t lpn, flushing_lpn;
    uint64_t sublat = 0, maxlat = 0;
    uint64_t ezrwa = 0, elba = 0, eizfr = 0; // IZFR: Implicit Zone Flush Region
    NvmeNamespace *ns = req->ns;
    NvmeZone *zone = zns_get_zone_by_slba(ns, lba);
    bool flush = false;
    int i;
    int wcidx = zns_get_wcidx(zns);

    if (zone->d.za & NVME_ZA_ZRWA_VALID){
        ezrwa = zone->w_ptr + n->zns->zrwas - 1;
        eizfr = zone->w_ptr + (2 * n->zns->zrwas) - 1;
        elba = lba + nlb;
        if (elba > ezrwa) {
            flush = true;
        }
    }else{
        flush = true;
    }

    if(flush == true && wcidx == -1)
    {
        //need flush
        wcidx = 0;
        uint64_t t_used = zns->cache.write_cache[wcidx].used;
        for(i = 0; i < zns->cache.num_wc; i++)
        {
            if(zns->cache.write_cache[i].used == 0)
            {
                t_used = 0;
                wcidx = i; //free wc！
                break;
            }
            if(zns->cache.write_cache[i].used > t_used)
            {
                t_used = zns->cache.write_cache[i].used;
                wcidx = i;
            }
        }
        if (t_used) maxlat = zns_wc_flush(zns, wcidx, USER_IO, req->stime);
        zns->cache.write_cache[wcidx].sblk = zns->active_zone;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        plba = lpn * secs_per_pg;
        if ((zone->d.za & NVME_ZA_ZRWA_VALID) && plba < ezrwa){
            // In-ZRWA writes will not write to SSD page cache
            sublat += SRAM_WRITE_LATENCY_NS;
            write_log("ZRWA Write to lpn 0x%"PRIx64"\n", lpn);
        }else if ((zone->d.za & NVME_ZA_ZRWA_VALID) && plba > ezrwa && plba < eizfr){
            // Implicit flush
            
            flushing_lpn = (plba - n->zns->zrwas) / secs_per_pg;
            write_log("ZRWA Implicit Flush to lpn 0x%"PRIx64", current lpn: 0x%"PRIx64"\n", flushing_lpn, lpn);
            ppa = get_new_page(zns);
            ppa.g.pl = 0;
            ppa.g.pg = get_blk(zns, &ppa)->page_wp;
            get_blk(zns, &ppa)->page_wp++;
            
            oldppa = get_maptbl_ent(zns, flushing_lpn);
            if (mapped_ppa(&oldppa)) {
                /* FIXME: Misao: update old page information*/
            }
            ppa.g.spg = subpage;
            /* update maptbl */
            set_maptbl_ent(zns, flushing_lpn, &ppa);
            // ftl_debug("[F] lpn:\t%lu\t-->ch:\t%u\tlun:\t%u\tpl:\t%u\tblk:\t%u\tpg:\t%u\tsubpg:\t%u\tlat\t%lu\n",lpn,ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg,sublat);
            //FIXME Misao: identify padding page
            if(ppa.g.V)
            {
                struct nand_cmd swr;
                swr.type = USER_IO;
                swr.cmd = NAND_WRITE;
                swr.stime = req->stime;
                /* get latency statistics */
                sublat = zns_advance_status(zns, &ppa, &swr);
                // Assume DRAM latency is hidden by Die latency
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }


        }else{
            if(zns->cache.write_cache[wcidx].used==zns->cache.write_cache[wcidx].cap)
            {
                ftl_debug("[W] flush wc %d (%u/%u)\n",wcidx,(int)zns->cache.write_cache[wcidx].used,(int)zns->cache.write_cache[wcidx].cap);
                sublat = zns_wc_flush(zns,wcidx, USER_IO, req->stime);
                ftl_debug("[W] flush lat: %u\n", (int)sublat);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
                sublat = 0;
            }
            zns->cache.write_cache[wcidx].lpns[zns->cache.write_cache[wcidx].used++]=lpn;
            sublat += SRAM_WRITE_LATENCY_NS; //Simplified timing emulation
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            ftl_debug("[W] lpn:\t%lu\t-->wc cache:%u, used:%u\n",lpn,(int)wcidx,(int)zns->cache.write_cache[wcidx].used);
        }
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    return maxlat;
}


static uint64_t zns_raum_flush(FemuCtrl *n, struct zns_ssd *zns, NvmeRequest *req)
{
    struct ppa ppa, oldppa;
    NvmePassthruCmd *pcmd = (NvmePassthruCmd *)&req->cmd; 
    // NvmeNamespace zns_ns = n->namespaces[0];
    // NvmeNamespace raum_ns = n->namespaces[1];
    req->status = NVME_SUCCESS;
    uint64_t sublat = 0, maxlat = 0;
    uint32_t flash_slba_low = le32_to_cpu(pcmd->cdw10);
    uint32_t flash_slba_high = le32_to_cpu(pcmd->cdw11);
    uint64_t flash_slba = (uint64_t)flash_slba_high << 32 | flash_slba_low;
    uint32_t nlb = le32_to_cpu(pcmd->cdw12);

    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE / zns->lbasz;
    uint64_t start_lpn = flash_slba / secs_per_pg;
    uint64_t end_lpn = (flash_slba + nlb - 1) / secs_per_pg;

    // uint32_t raum_slba_low = le32_to_cpu(pcmd->cdw14);
    // uint32_t raum_slba_high = le32_to_cpu(pcmd->cdw15);
    // uint64_t raum_slba = (uint64_t)raum_slba_high << 32 | raum_slba_low;

    uint64_t lpn;

    
    int subpage = 0;
    // int wcidx = zns_get_wcidx(zns);

    // NvmeZone *zone = zns_get_zone_by_slba(zns_ns, flash_slba);

    // if(wcidx == -1)
    // {
    //     //need flush
    //     wcidx = 0;
    //     uint64_t t_used = zns->cache.write_cache[wcidx].used;
    //     for(i = 0; i < zns->cache.num_wc; i++)
    //     {
    //         if(zns->cache.write_cache[i].used == 0)
    //         {
    //             t_used = 0;
    //             wcidx = i; //free wc！
    //             break;
    //         }
    //         if(zns->cache.write_cache[i].used > t_used)
    //         {
    //             t_used = zns->cache.write_cache[i].used;
    //             wcidx = i;
    //         }
    //     }
    //     if (t_used) maxlat = zns_wc_flush(zns, wcidx, USER_IO, req->stime);
    //     zns->cache.write_cache[wcidx].sblk = zns->active_zone;
    // }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        // write_log("RAUM flush to lpn 0x%"PRIx64", current lpn: 0x%"PRIx64"\n", flushing_lpn, lpn);
        ppa = get_new_page(zns);
        ppa.g.pl = 0;
        ppa.g.pg = get_blk(zns, &ppa)->page_wp;
        get_blk(zns, &ppa)->page_wp++;
        
        oldppa = get_maptbl_ent(zns, lpn);
        if (mapped_ppa(&oldppa)) {
            /* FIXME: Misao: update old page information*/
        }
        ppa.g.spg = subpage;
        /* update maptbl */
        set_maptbl_ent(zns, lpn, &ppa);
        // ftl_debug("[F] lpn:\t%lu\t-->ch:\t%u\tlun:\t%u\tpl:\t%u\tblk:\t%u\tpg:\t%u\tsubpg:\t%u\tlat\t%lu\n",lpn,ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg,sublat);
        //FIXME Misao: identify padding page
        if(ppa.g.V)
        {
            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;
            /* get latency statistics */
            sublat = zns_advance_status(zns, &ppa, &swr);
            sublat += SRAM_READ_LATENCY_NS;
            // Assume DRAM latency is hidden by Die latency
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
    }

    return maxlat;
}

static uint64_t zns_zone_mgmt_send_with_latency(FemuCtrl *n, struct zns_ssd *zns, NvmeRequest *req)
{
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint8_t action;
    uint64_t elba;
    uint32_t zone_idx;

    zns_get_mgmt_zone_slba_idx(n, cmd, &elba, &zone_idx);

    NvmeZone *zone = &n->zone_array[zone_idx];

    uint64_t sublat = 0, maxlat = 0;

    uint64_t lba = zone->w_ptr;
    uint64_t lpn = 0;
    // uint32_t nlb = elba - lba + 1;
    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE / zns->lbasz;
    uint64_t start_lpn = lba / secs_per_pg;
    uint64_t end_lpn = elba / secs_per_pg;

    // TODO: check elba in print
    // See if it is aligned with n->zns->zrwafg or n->zns->zrwafg - 1

    struct ppa ppa, oldppa;

    action = dw13 & 0xff;

    req->status = NVME_SUCCESS;

    switch (action) {
        case NVME_ZONE_ACTION_ZRWA_FLUSH:
            for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
                ppa = get_new_page(zns);
                ppa.g.pl = 0;
                ppa.g.pg = get_blk(zns, &ppa)->page_wp;
                get_blk(zns, &ppa)->page_wp++;
                oldppa = get_maptbl_ent(zns, lpn);
                if (mapped_ppa(&oldppa)) {
                    /* FIXME: Misao: update old page information*/
                }
                ppa.g.spg = 0;
                /* update maptbl */
                set_maptbl_ent(zns, lpn, &ppa);
                // ftl_debug("[F] lpn:\t%lu\t-->ch:\t%u\tlun:\t%u\tpl:\t%u\tblk:\t%u\tpg:\t%u\tsubpg:\t%u\tlat\t%lu\n",lpn,ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg,sublat);
            
            
                //FIXME Misao: identify padding page
                if(ppa.g.V)
                {
                    struct nand_cmd swr;
                    swr.type = USER_IO;
                    swr.cmd = NAND_WRITE;
                    swr.stime = req->stime;
                    /* get latency statistics */
                    sublat = zns_advance_status(zns, &ppa, &swr);
                    maxlat = (sublat > maxlat) ? sublat : maxlat;
                }
            }
            return maxlat;

            
            
            // return zns_zone_mgmt_send_zrwa_flush(n, zone, slba, req);
        default:
            return 0;
    }

}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct zns_ssd *zns = n->zns;
    NvmeRequest *req = NULL;
    uint64_t lat = 0, stime = 0;
    int rc;
    int i;
    int wcidx = 0;

    while (!*(zns->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    zns->to_ftl = n->to_ftl;
    zns->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!zns->to_ftl[i] || !femu_ring_count(zns->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(zns->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            if (req->ns->id == 1){
                switch (req->cmd.opcode) {
                    // Fix bug: zone append not respecting configured delay
                    case NVME_CMD_ZONE_APPEND:
                        /* Fall through */
                    case NVME_CMD_WRITE:
                        lat = zns_write(n, zns, req);
                        break;
                    case NVME_CMD_READ:
                        lat = zns_read(n, zns, req);
                        break;
                    case NVME_CMD_DSM:
                        lat = 0;
                        break;
                    case NVME_CMD_ZONE_MGMT_SEND:
                        lat = zns_zone_mgmt_send_with_latency(n, zns, req);
                    default:
                        //ftl_err("FTL received unkown request type, ERROR\n");
                        ;
                }
                req->reqlat = lat;
                req->expire_time += lat;
            }else if(req->ns->id == 2){
                // URWA, use SRAM Latency
                
                if (req->cmd.opcode == NVME_CMD_FLUSH_RAUM){
                    lat = zns_raum_flush(n, zns, req);
                    req->reqlat = lat;
                    req->expire_time += lat;
                }else{
                    req->reqlat = SRAM_WRITE_LATENCY_NS;
                    req->expire_time += SRAM_WRITE_LATENCY_NS;
                }
                
            }

            rc = femu_ring_enqueue(zns->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            // Flush write buffer in background
            stime = req->expire_time;
            for(wcidx = 0; wcidx < zns->cache.num_wc; wcidx++){
                stime += zns_wc_flush(zns, wcidx, USER_IO, stime);
            }

        }
    }

    return NULL;
}
