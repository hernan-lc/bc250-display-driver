import re, sys, os
L = sys.argv[1]; out = sys.argv[2]
def load(fn):
    d = {}
    for line in open(os.path.join(L, fn), encoding='utf-8', errors='replace'):
        m = re.match(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)L?\b', line)
        if m: d[m.group(1)] = int(m.group(2), 0)
    return d

# IP segment bases (dwords) from cyan_skillfish_ip_offset.h
BASES = {
 'GC':     [0x00001260, 0x0000A000],
 'MMHUB':  [0x0001A000],
 'OSSSYS': [0x000010A0],
 'NBIO':   [0x00000000, 0x00000014, 0x00000D20, 0x00010400],
 'MP1':    [0x00016000],
 'MP0':    [0x00016000],
 'THM':    [0x00016600],
 'HDP':    [0x00000F20],
}
groups = [
 ('GC', 'include_asic_reg_gc_gc_10_1_0_offset.h', 'include_asic_reg_gc_gc_10_1_0_sh_mask.h', [
   'GRBM_STATUS','GRBM_STATUS2','CP_STAT','RLC_STAT','GB_ADDR_CONFIG','CC_GC_SHADER_ARRAY_CONFIG',
   'GC_USER_SHADER_ARRAY_CONFIG','SCRATCH_REG0','CP_ME_CNTL','CP_MEC_CNTL',
   'GCMC_VM_FB_LOCATION_BASE','GCMC_VM_FB_LOCATION_TOP','GCMC_VM_FB_OFFSET','GCMC_VM_AGP_BASE','GCMC_VM_AGP_BOT','GCMC_VM_AGP_TOP',
   'GCMC_VM_SYSTEM_APERTURE_LOW_ADDR','GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR',
   'GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB','GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB',
   'GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32','GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32',
   'GCVM_L2_PROTECTION_FAULT_STATUS','GCVM_L2_PROTECTION_FAULT_ADDR_LO32','GCVM_L2_PROTECTION_FAULT_ADDR_HI32',
   'GCMC_VM_MX_L1_TLB_CNTL','GCVM_L2_CNTL','GCVM_L2_CNTL2','GCVM_L2_CNTL3','GCVM_L2_CNTL4','GCVM_L2_CNTL5',
   'GCVM_CONTEXT0_CNTL','GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32','GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32',
   'GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32','GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32',
   'GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32','GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32',
   'GCVM_INVALIDATE_ENG17_REQ','GCVM_INVALIDATE_ENG17_ACK','GCVM_INVALIDATE_ENG17_SEM',
   'GCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32','GCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32',
   'SDMA0_F32_CNTL','SDMA0_CNTL','SDMA0_UCODE_ADDR','SDMA0_UCODE_DATA','SDMA0_STATUS_REG',
   'SDMA0_GFX_RB_CNTL','SDMA0_GFX_RB_BASE','SDMA0_GFX_RB_BASE_HI','SDMA0_GFX_RB_RPTR','SDMA0_GFX_RB_RPTR_HI',
   'SDMA0_GFX_RB_WPTR','SDMA0_GFX_RB_WPTR_HI','SDMA0_GFX_RB_RPTR_ADDR_LO','SDMA0_GFX_RB_RPTR_ADDR_HI',
   'SDMA0_GFX_RB_WPTR_POLL_CNTL','SDMA0_GFX_RB_WPTR_POLL_ADDR_LO','SDMA0_GFX_RB_WPTR_POLL_ADDR_HI',
   'SDMA0_GFX_IB_CNTL','SDMA0_GFX_DOORBELL','SDMA0_GFX_MINOR_PTR_UPDATE','SDMA0_UTCL1_CNTL','SDMA0_UTCL1_PAGE',
   'SDMA0_SEM_WAIT_FAIL_TIMER_CNTL','SDMA0_CHICKEN_BITS','SDMA0_GB_ADDR_CONFIG','SDMA0_GB_ADDR_CONFIG_READ',
   # M4b: RLC + CP graphics
   'RLC_CNTL','RLC_CGCG_CGLS_CTRL','RLC_PG_CNTL','RLC_GPM_UCODE_ADDR','RLC_GPM_UCODE_DATA','RLC_CSIB_ADDR_HI','RLC_CSIB_ADDR_LO',
   'RLC_CSIB_LENGTH','GRBM_SOFT_RESET','GRBM_GFX_CNTL','GRBM_GFX_INDEX',
   'CP_PFP_IC_OP_CNTL','CP_PFP_IC_BASE_CNTL','CP_PFP_IC_BASE_LO','CP_PFP_IC_BASE_HI','CP_HYP_PFP_UCODE_ADDR','CP_HYP_PFP_UCODE_DATA',
   'CP_CE_IC_OP_CNTL','CP_CE_IC_BASE_CNTL','CP_CE_IC_BASE_LO','CP_CE_IC_BASE_HI','CP_HYP_CE_UCODE_ADDR','CP_HYP_CE_UCODE_DATA',
   'CP_ME_IC_OP_CNTL','CP_ME_IC_BASE_CNTL','CP_ME_IC_BASE_LO','CP_ME_IC_BASE_HI','CP_HYP_ME_UCODE_ADDR','CP_HYP_ME_UCODE_DATA',
   'CP_MAX_CONTEXT','CP_DEVICE_ID','CP_RB_WPTR_DELAY','CP_RB_VMID','CP_RB0_CNTL','CP_RB0_WPTR','CP_RB0_WPTR_HI','CP_RB0_RPTR',
   'CP_RB0_RPTR_ADDR','CP_RB0_RPTR_ADDR_HI','CP_RB_WPTR_POLL_ADDR_LO','CP_RB_WPTR_POLL_ADDR_HI','CP_RB0_BASE','CP_RB0_BASE_HI',
   'CP_RB_ACTIVE','CP_RB_DOORBELL_CONTROL','CP_RB_WPTR_POLL_CNTL','CP_INT_CNTL_RING0',
   # M4d: MEC compute queue (HQD)
   'CP_PQ_WPTR_POLL_CNTL','CP_HQD_ACTIVE','CP_HQD_DEQUEUE_REQUEST','CP_HQD_PQ_RPTR','CP_HQD_PQ_WPTR_LO','CP_HQD_PQ_WPTR_HI',
   'CP_HQD_PQ_DOORBELL_CONTROL','CP_HQD_EOP_BASE_ADDR','CP_HQD_EOP_BASE_ADDR_HI','CP_HQD_EOP_CONTROL','CP_MQD_BASE_ADDR',
   'CP_MQD_BASE_ADDR_HI','CP_MQD_CONTROL','CP_HQD_PQ_BASE','CP_HQD_PQ_BASE_HI','CP_HQD_PQ_CONTROL','CP_HQD_PQ_RPTR_REPORT_ADDR',
   'CP_HQD_PQ_RPTR_REPORT_ADDR_HI','CP_HQD_PQ_WPTR_POLL_ADDR','CP_HQD_PQ_WPTR_POLL_ADDR_HI','CP_HQD_VMID','CP_HQD_PERSISTENT_STATE',
   'CP_HQD_IB_CONTROL','CP_HQD_QUANTUM','CP_CPC_STATUS','CP_MEC1_INSTR_PNTR',
 ], [
   'GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK','GCMC_VM_FB_LOCATION_TOP__FB_TOP_MASK',
   'GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK','GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE__SHIFT',
   'GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK','GCMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK',
   'GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK','GCMC_VM_MX_L1_TLB_CNTL__MTYPE__SHIFT','GCMC_VM_MX_L1_TLB_CNTL__MTYPE_MASK',
   'GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK','GCVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK',
   'GCVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK','GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT',
   'GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK',
   'GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK','GCVM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK',
   'GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK','GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK',
   'GCVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK',
   'GCVM_INVALIDATE_ENG17_REQ__PER_VMID_INVALIDATE_REQ__SHIFT','GCVM_INVALIDATE_ENG17_REQ__FLUSH_TYPE__SHIFT',
   'GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES_MASK','GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE0_MASK',
   'GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE1_MASK','GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE2_MASK',
   'GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES_MASK',
   'SDMA0_F32_CNTL__HALT_MASK','SDMA0_CNTL__TRAP_ENABLE_MASK','SDMA0_CNTL__UTC_L1_ENABLE_MASK',
   'SDMA0_CNTL__AUTO_CTXSW_ENABLE_MASK','SDMA0_CNTL__MIDCMD_PREEMPT_ENABLE_MASK',
   'SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK','SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT','SDMA0_GFX_RB_CNTL__RB_SIZE_MASK',
   'SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK','SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK',
   'SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK','SDMA0_GFX_DOORBELL__ENABLE_MASK',
   'SDMA0_UTCL1_CNTL__RESP_MODE__SHIFT','SDMA0_UTCL1_CNTL__RESP_MODE_MASK','SDMA0_UTCL1_CNTL__REDO_DELAY__SHIFT',
   'SDMA0_UTCL1_CNTL__REDO_DELAY_MASK','SDMA0_STATUS_REG__IDLE_MASK',
   'RLC_CNTL__RLC_ENABLE_F32_MASK','GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK','CP_ME_CNTL__ME_HALT_MASK','CP_ME_CNTL__PFP_HALT_MASK',
   'CP_ME_CNTL__CE_HALT_MASK','CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_MASK','CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK',
   'CP_CE_IC_OP_CNTL__INVALIDATE_CACHE_MASK','CP_CE_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK',
   'CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_MASK','CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK',
   'CP_PFP_IC_BASE_CNTL__VMID_MASK','CP_PFP_IC_BASE_CNTL__CACHE_POLICY_MASK','CP_PFP_IC_BASE_CNTL__EXE_DISABLE_MASK','CP_PFP_IC_BASE_CNTL__ADDRESS_CLAMP_MASK',
   'CP_RB0_CNTL__RB_BUFSZ_MASK','CP_RB0_CNTL__RB_BLKSZ_MASK','CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK','GRBM_GFX_CNTL__PIPEID_MASK','CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK',
   'CP_HQD_EOP_CONTROL__EOP_SIZE_MASK','CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK','CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK',
   'CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK','CP_HQD_PQ_CONTROL__PRIV_STATE_MASK','CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK',
   'CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK','CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE_MASK','CP_PQ_WPTR_POLL_CNTL__EN_MASK',
   'GRBM_GFX_CNTL__MEID_MASK','GRBM_GFX_CNTL__QUEUEID_MASK','CP_MEC_CNTL__MEC_ME1_HALT_MASK','CP_MEC_CNTL__MEC_ME2_HALT_MASK',
   'CP_HQD_QUANTUM__QUANTUM_EN_MASK','CP_HQD_QUANTUM__QUANTUM_SCALE_MASK','CP_HQD_QUANTUM__QUANTUM_DURATION_MASK',
 ]),
 ('MMHUB', 'include_asic_reg_mmhub_mmhub_2_0_0_offset.h', None, [
   'MMMC_VM_FB_LOCATION_BASE','MMMC_VM_FB_LOCATION_TOP','MMMC_VM_FB_OFFSET','MMMC_VM_AGP_BOT','MMMC_VM_AGP_TOP',
   'MMMC_VM_SYSTEM_APERTURE_LOW_ADDR','MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR'], []),
 ('OSSSYS', 'include_asic_reg_oss_osssys_5_0_0_offset.h', 'include_asic_reg_oss_osssys_5_0_0_sh_mask.h', [
   'IH_RB_CNTL','IH_RB_BASE','IH_RB_BASE_HI','IH_RB_RPTR','IH_RB_WPTR','IH_RB_WPTR_ADDR_LO','IH_RB_WPTR_ADDR_HI',
   'IH_DOORBELL_RPTR','IH_CNTL','IH_CHICKEN'], [
   'IH_RB_CNTL__RB_ENABLE_MASK','IH_RB_CNTL__RB_SIZE__SHIFT','IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK',
   'IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK','IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK','IH_RB_CNTL__RPTR_REARM_MASK',
   'IH_RB_CNTL__MC_SPACE__SHIFT','IH_RB_CNTL__MC_SPACE_MASK','IH_RB_WPTR__RB_OVERFLOW_MASK','IH_RB_WPTR__OFFSET_MASK',
   'IH_CHICKEN__MC_SPACE_GPA_ENABLE_MASK','IH_RB_CNTL__ENABLE_INTR_MASK','IH_RB_CNTL__MC_SNOOP_MASK','IH_RB_CNTL__RB_GPU_TS_ENABLE_MASK']),
 ('NBIO', 'include_asic_reg_nbio_nbio_2_3_offset.h', None, [
   'PCIE_INDEX2','PCIE_DATA2','INTERRUPT_CNTL','INTERRUPT_CNTL2','RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE','BIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL'], []),
 ('HDP', 'include_asic_reg_hdp_hdp_5_0_0_offset.h', None, ['HDP_READ_CACHE_INVALIDATE'], []),
 ('MP1', 'include_asic_reg_mp_mp_11_0_offset.h', None, [
   'MP1_SMN_C2PMSG_66','MP1_SMN_C2PMSG_82','MP1_SMN_C2PMSG_90'], []),
 ('MP0', 'include_asic_reg_mp_mp_11_0_offset.h', None, [
   'MP0_SMN_C2PMSG_64','MP0_SMN_C2PMSG_67','MP0_SMN_C2PMSG_69','MP0_SMN_C2PMSG_70','MP0_SMN_C2PMSG_71','MP0_SMN_C2PMSG_81'], []),
 ('THM', 'include_asic_reg_thm_thm_11_0_2_offset.h', 'include_asic_reg_thm_thm_11_0_2_sh_mask.h', [
   'CG_MULT_THERMAL_STATUS'], ['CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT','CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK']),
]
lines = ['// Generated from Linux amdgpu register headers (MIT license, Copyright Advanced Micro Devices, Inc.).',
         '// Offsets are absolute MMIO dword offsets for Cyan Skillfish (IP bases from cyan_skillfish_ip_offset.h).',
         '// Regenerate with tools/genregs.py; do not edit by hand.', '#pragma once', '']
missing = []
for ip, off, mask, regs, fields in groups:
    o = load(off); m = load(mask) if mask else {}
    if ip == 'GC': o.update({k: v for k, v in load('amdgpu_gfx_v10_0.c').items() if k.startswith('mmCP_HYP_')})
    lines.append(f'// ---- {ip} ----')
    for r in regs:
        k = 'mm' + r
        if k not in o or k + '_BASE_IDX' not in o:
            missing.append(r); continue
        val = BASES[ip][o[k + '_BASE_IDX']] + o[k]
        lines.append(f'#define BC250_{r:<48} 0x{val:05X}u')
    for f in fields:
        if f not in m: missing.append(f); continue
        lines.append(f'#define BC250_{f:<64} 0x{m[f]:X}u')
    lines.append('')
# ---- GC golden settings for Cyan Skillfish (gfx_v10_0.c) ----
gco = load('include_asic_reg_gc_gc_10_1_0_offset.h')
src = open(os.path.join(L, 'amdgpu_gfx_v10_0.c'), encoding='utf-8', errors='replace').read()
blk = re.search(r'golden_settings_gc_10_0_cyan_skillfish\[\] = \{(.*?)\};', src, re.S).group(1)
lines.append('// ---- GC golden settings (golden_settings_gc_10_0_cyan_skillfish) : { reg, and_mask, or_mask } ----')
lines.append('#define BC250_GOLDEN_GC_INIT { \\')
for m in re.finditer(r'SOC15_REG_GOLDEN_VALUE\(GC,\s*0,\s*mm(\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', blk):
    r = 'mm' + m.group(1)
    if r not in gco: missing.append(m.group(1)); continue
    val = BASES['GC'][gco[r + '_BASE_IDX']] + gco[r]
    lines.append(f'    {{ 0x{val:05X}u, {m.group(2)}u, {m.group(3)}u }}, /* {m.group(1)} */ \\')
lines.append('}')
lines.append('')

# ---- Clear-state buffer (clearstate_gfx10.h, amdgpu_gfx_csb_*), without PA_SC_TILE_STEERING_OVERRIDE ----
cs = open(os.path.join(L, 'amdgpu_clearstate_gfx10.h'), encoding='utf-8', errors='replace').read()
cs = re.sub(r'//[^\n]*', '', cs)
arrays = {}
for m in re.finditer(r'static const unsigned int (\w+)\[\] =\s*\{(.*?)\};', cs, re.S):
    arrays[m.group(1)] = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', m.group(2))]
ext = re.search(r'gfx10_SECT_CONTEXT_defs\[\] =\s*\{(.*?)\};', cs, re.S).group(1)
def P3(op, n): return (3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16)
csb = [P3(0x4A, 0), 2 << 28, P3(0x28, 1), 0x80000000, 0x80000000]
for m in re.finditer(r'\{\s*(\w+),\s*(0x[0-9a-fA-F]+|\d+),\s*(\d+)\s*\}', ext):
    if m.group(1) == '0': continue
    data = arrays[m.group(1)]; idx = int(m.group(2), 0); cnt = int(m.group(3))
    assert len(data) == cnt, (m.group(1), len(data), cnt)
    csb += [P3(0x69, cnt), idx - 0xA000] + data[:cnt]
csb += [P3(0x4A, 0), 3 << 28, P3(0x12, 0), 0]
lines.append(f'// ---- Clear-state buffer ({len(csb)} dwords) ----')
lines.append(f'#define BC250_CSB_DWORDS {len(csb)}u')
lines.append('#define BC250_CSB_INIT { \\')
for i in range(0, len(csb), 8):
    lines.append('    ' + ', '.join(f'0x{v:08X}u' for v in csb[i:i+8]) + ', \\')
lines.append('}')
open(out, 'w', newline='\n').write('\n'.join(lines))
print('missing:', missing)
