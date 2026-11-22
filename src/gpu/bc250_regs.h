// Generated from Linux amdgpu register headers (MIT license, Copyright Advanced Micro Devices, Inc.).
// Offsets are absolute MMIO dword offsets for Cyan Skillfish (IP bases from cyan_skillfish_ip_offset.h).
// Regenerate with tools/genregs.py; do not edit by hand.
#pragma once

// ---- GC ----
#define BC250_GRBM_STATUS                                      0x02004u
#define BC250_GRBM_STATUS2                                     0x02002u
#define BC250_CP_STAT                                          0x021A0u
#define BC250_RLC_STAT                                         0x0EC04u
#define BC250_GB_ADDR_CONFIG                                   0x0263Eu
#define BC250_CC_GC_SHADER_ARRAY_CONFIG                        0x0226Fu
#define BC250_GC_USER_SHADER_ARRAY_CONFIG                      0x02270u
#define BC250_SCRATCH_REG0                                     0x0C040u
#define BC250_CP_ME_CNTL                                       0x021B6u
#define BC250_CP_MEC_CNTL                                      0x0208Du
#define BC250_GCMC_VM_FB_LOCATION_BASE                         0x02980u
#define BC250_GCMC_VM_FB_LOCATION_TOP                          0x02981u
#define BC250_GCMC_VM_FB_OFFSET                                0x0296Bu
#define BC250_GCMC_VM_AGP_BASE                                 0x02984u
#define BC250_GCMC_VM_AGP_BOT                                  0x02983u
#define BC250_GCMC_VM_AGP_TOP                                  0x02982u
#define BC250_GCMC_VM_SYSTEM_APERTURE_LOW_ADDR                 0x02985u
#define BC250_GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR                0x02986u
#define BC250_GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB         0x0296Cu
#define BC250_GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB         0x0296Du
#define BC250_GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32       0x0284Fu
#define BC250_GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32       0x02850u
#define BC250_GCVM_L2_PROTECTION_FAULT_STATUS                  0x0284Cu
#define BC250_GCVM_L2_PROTECTION_FAULT_ADDR_LO32               0x0284Du
#define BC250_GCVM_L2_PROTECTION_FAULT_ADDR_HI32               0x0284Eu
#define BC250_GCMC_VM_MX_L1_TLB_CNTL                           0x02987u
#define BC250_GCVM_L2_CNTL                                     0x02840u
#define BC250_GCVM_L2_CNTL2                                    0x02841u
#define BC250_GCVM_L2_CNTL3                                    0x02842u
#define BC250_GCVM_L2_CNTL4                                    0x02858u
#define BC250_GCVM_L2_CNTL5                                    0x02861u
#define BC250_GCVM_CONTEXT0_CNTL                               0x02880u
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32          0x028EBu
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32          0x028ECu
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32         0x0290Bu
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32         0x0290Cu
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32           0x0292Bu
#define BC250_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32           0x0292Cu
#define BC250_GCVM_INVALIDATE_ENG17_REQ                        0x028B4u
#define BC250_GCVM_INVALIDATE_ENG17_ACK                        0x028C6u
#define BC250_GCVM_INVALIDATE_ENG17_SEM                        0x028A2u
#define BC250_GCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32             0x028C7u
#define BC250_GCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32             0x028C8u
#define BC250_SDMA0_F32_CNTL                                   0x0128Au
#define BC250_SDMA0_CNTL                                       0x0127Cu
#define BC250_SDMA0_UCODE_ADDR                                 0x0F880u
#define BC250_SDMA0_UCODE_DATA                                 0x0F881u
#define BC250_SDMA0_STATUS_REG                                 0x01285u
#define BC250_SDMA0_GFX_RB_CNTL                                0x012E0u
#define BC250_SDMA0_GFX_RB_BASE                                0x012E1u
#define BC250_SDMA0_GFX_RB_BASE_HI                             0x012E2u
#define BC250_SDMA0_GFX_RB_RPTR                                0x012E3u
#define BC250_SDMA0_GFX_RB_RPTR_HI                             0x012E4u
#define BC250_SDMA0_GFX_RB_WPTR                                0x012E5u
#define BC250_SDMA0_GFX_RB_WPTR_HI                             0x012E6u
#define BC250_SDMA0_GFX_RB_RPTR_ADDR_LO                        0x012E9u
#define BC250_SDMA0_GFX_RB_RPTR_ADDR_HI                        0x012E8u
#define BC250_SDMA0_GFX_RB_WPTR_POLL_CNTL                      0x012E7u
#define BC250_SDMA0_GFX_RB_WPTR_POLL_ADDR_LO                   0x01313u
#define BC250_SDMA0_GFX_RB_WPTR_POLL_ADDR_HI                   0x01312u
#define BC250_SDMA0_GFX_IB_CNTL                                0x012EAu
#define BC250_SDMA0_GFX_DOORBELL                               0x012F2u
#define BC250_SDMA0_GFX_MINOR_PTR_UPDATE                       0x01315u
#define BC250_SDMA0_UTCL1_CNTL                                 0x0129Cu
#define BC250_SDMA0_UTCL1_PAGE                                 0x012A8u
#define BC250_SDMA0_SEM_WAIT_FAIL_TIMER_CNTL                   0x01281u
#define BC250_SDMA0_CHICKEN_BITS                               0x0127Du
#define BC250_SDMA0_GB_ADDR_CONFIG                             0x0127Eu
#define BC250_SDMA0_GB_ADDR_CONFIG_READ                        0x0127Fu
#define BC250_GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK                           0xFFFFFFu
#define BC250_GCMC_VM_FB_LOCATION_TOP__FB_TOP_MASK                             0xFFFFFFu
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK                       0x1u
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE__SHIFT                0x3u
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK                  0x18u
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK        0x40u
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK     0x20u
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__MTYPE__SHIFT                             0xBu
#define BC250_GCMC_VM_MX_L1_TLB_CNTL__MTYPE_MASK                               0x3800u
#define BC250_GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK                               0x1u
#define BC250_GCVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK                 0x2u
#define BC250_GCVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK      0x800u
#define BC250_GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT               0x13u
#define BC250_GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK                 0x180000u
#define BC250_GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK                       0x1u
#define BC250_GCVM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK                          0x2u
#define BC250_GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK                          0x1u
#define BC250_GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK                        0x6u
#define BC250_GCVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK  0x80u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__PER_VMID_INVALIDATE_REQ__SHIFT        0x0u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__FLUSH_TYPE__SHIFT                     0x10u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES_MASK               0x80000u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE0_MASK               0x100000u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE1_MASK               0x200000u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE2_MASK               0x400000u
#define BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES_MASK               0x800000u
#define BC250_SDMA0_F32_CNTL__HALT_MASK                                        0x1u
#define BC250_SDMA0_CNTL__TRAP_ENABLE_MASK                                     0x1u
#define BC250_SDMA0_CNTL__UTC_L1_ENABLE_MASK                                   0x2u
#define BC250_SDMA0_CNTL__AUTO_CTXSW_ENABLE_MASK                               0x40000u
#define BC250_SDMA0_CNTL__MIDCMD_PREEMPT_ENABLE_MASK                           0x20u
#define BC250_SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK                                0x1u
#define BC250_SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT                                0x1u
#define BC250_SDMA0_GFX_RB_CNTL__RB_SIZE_MASK                                  0x3Eu
#define BC250_SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK                    0x1000u
#define BC250_SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK                                0x1u
#define BC250_SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                0x4u
#define BC250_SDMA0_GFX_DOORBELL__ENABLE_MASK                                  0x10000000u
#define BC250_SDMA0_UTCL1_CNTL__RESP_MODE__SHIFT                               0x9u
#define BC250_SDMA0_UTCL1_CNTL__RESP_MODE_MASK                                 0xE00u
#define BC250_SDMA0_UTCL1_CNTL__REDO_DELAY__SHIFT                              0x1u
#define BC250_SDMA0_UTCL1_CNTL__REDO_DELAY_MASK                                0x3Eu
#define BC250_SDMA0_STATUS_REG__IDLE_MASK                                      0x1u

// ---- MMHUB ----
#define BC250_MMMC_VM_FB_LOCATION_BASE                         0x1A86Cu
#define BC250_MMMC_VM_FB_LOCATION_TOP                          0x1A86Du
#define BC250_MMMC_VM_FB_OFFSET                                0x1A857u
#define BC250_MMMC_VM_AGP_BOT                                  0x1A86Fu
#define BC250_MMMC_VM_AGP_TOP                                  0x1A86Eu
#define BC250_MMMC_VM_SYSTEM_APERTURE_LOW_ADDR                 0x1A871u
#define BC250_MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR                0x1A872u

// ---- OSSSYS ----
#define BC250_IH_RB_CNTL                                       0x01120u
#define BC250_IH_RB_BASE                                       0x01121u
#define BC250_IH_RB_BASE_HI                                    0x01122u
#define BC250_IH_RB_RPTR                                       0x01123u
#define BC250_IH_RB_WPTR                                       0x01124u
#define BC250_IH_RB_WPTR_ADDR_LO                               0x01126u
#define BC250_IH_RB_WPTR_ADDR_HI                               0x01125u
#define BC250_IH_DOORBELL_RPTR                                 0x01127u
#define BC250_IH_CNTL                                          0x01160u
#define BC250_IH_CHICKEN                                       0x0122Cu
#define BC250_IH_RB_CNTL__RB_ENABLE_MASK                                       0x1u
#define BC250_IH_RB_CNTL__RB_SIZE__SHIFT                                       0x1u
#define BC250_IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK                           0x100u
#define BC250_IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK                            0x10000u
#define BC250_IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK                             0x80000000u
#define BC250_IH_RB_CNTL__RPTR_REARM_MASK                                      0x200000u
#define BC250_IH_RB_CNTL__MC_SPACE__SHIFT                                      0x1Cu
#define BC250_IH_RB_CNTL__MC_SPACE_MASK                                        0x70000000u
#define BC250_IH_RB_WPTR__RB_OVERFLOW_MASK                                     0x1u
#define BC250_IH_RB_WPTR__OFFSET_MASK                                          0x3FFFCu
#define BC250_IH_CHICKEN__MC_SPACE_GPA_ENABLE_MASK                             0x10u
#define BC250_IH_RB_CNTL__ENABLE_INTR_MASK                                     0x20000u
#define BC250_IH_RB_CNTL__MC_SNOOP_MASK                                        0x100000u
#define BC250_IH_RB_CNTL__RB_GPU_TS_ENABLE_MASK                                0x80u

// ---- NBIO ----
#define BC250_PCIE_INDEX2                                      0x0000Eu
#define BC250_PCIE_DATA2                                       0x0000Fu
#define BC250_INTERRUPT_CNTL                                   0x00E11u
#define BC250_INTERRUPT_CNTL2                                  0x00E12u
#define BC250_RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE                 0x00DE3u
#define BC250_BIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL           0x00E17u

// ---- HDP ----
#define BC250_HDP_READ_CACHE_INVALIDATE                        0x00FF1u

// ---- MP1 ----
#define BC250_MP1_SMN_C2PMSG_66                                0x16282u
#define BC250_MP1_SMN_C2PMSG_82                                0x16292u
#define BC250_MP1_SMN_C2PMSG_90                                0x1629Au

// ---- THM ----
#define BC250_CG_MULT_THERMAL_STATUS                           0x1665Fu
#define BC250_CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT                          0x9u
#define BC250_CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK                            0x3FE00u
