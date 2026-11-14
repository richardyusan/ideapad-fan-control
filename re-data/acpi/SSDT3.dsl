/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20251212 (64-bit version)
 * Copyright (c) 2000 - 2025 Intel Corporation
 * 
 * Disassembling to symbolic ASL+ operators
 *
 * Disassembly of SSDT3.dat
 *
 * Original Table Header:
 *     Signature        "SSDT"
 *     Length           0x0000053A (1338)
 *     Revision         0x01
 *     Checksum         0x7D
 *     OEM ID           "LENOVO"
 *     OEM Table ID     "Tpm2Tabl"
 *     OEM Revision     0x00009999 (39321)
 *     Compiler ID      "INTL"
 *     Compiler Version 0x20180313 (538444563)
 */
DefinitionBlock ("", "SSDT", 1, "LENOVO", "Tpm2Tabl", 0x00009999)
{
    Scope (\_SB)
    {
        Device (TPM)
        {
            Name (_HID, "MSFT0101" /* TPM 2.0 Security Device */)  // _HID: Hardware ID
            Name (_CID, "MSFT0101" /* TPM 2.0 Security Device */)  // _CID: Compatible ID
            Name (_STR, Unicode ("TPM 2.0 Device"))  // _STR: Description String
            OperationRegion (SMIP, SystemIO, 0xB0, 0x01)
            Field (SMIP, ByteAcc, NoLock, Preserve)
            {
                IOB0,   8
            }

            Name (PFPF, 0x00000001)
            OperationRegion (DBG1, SystemIO, 0x80, 0x02)
            Field (DBG1, WordAcc, NoLock, Preserve)
            {
                P80L,   16
            }

            OperationRegion (TNVS, SystemMemory, 0xCBB80000, 0x2A)
            Field (TNVS, AnyAcc, NoLock, Preserve)
            {
                PPIN,   8, 
                PPIP,   32, 
                PPRP,   32, 
                PPRQ,   32, 
                PPRM,   32, 
                LPPR,   32, 
                FRET,   32, 
                MCIN,   8, 
                MCIP,   32, 
                MORD,   32, 
                MRET,   32, 
                UCRQ,   32
            }

            OperationRegion (PFCA, SystemMemory, 0xFDE10510, 0xF0)
            Field (PFCA, DWordAcc, NoLock, Preserve)
            {
                HRTB,   32, 
                ERRR,   32, 
                CLFG,   32, 
                STA1,   32, 
                AccessAs (QWordAcc, 0x00), 
                PFIN,   64, 
                AccessAs (DWordAcc, 0x00), 
                CMSZ,   32, 
                AccessAs (QWordAcc, 0x00), 
                CMDB,   64, 
                AccessAs (DWordAcc, 0x00), 
                RPSZ,   32, 
                AccessAs (QWordAcc, 0x00), 
                RESP,   64
            }

            Name (_CRS, ResourceTemplate ()  // _CRS: Current Resource Settings
            {
                Memory32Fixed (ReadWrite,
                    0xCB45F000,         // Address Base
                    0x00004000,         // Address Length
                    )
                Memory32Fixed (ReadWrite,
                    0xCB463000,         // Address Base
                    0x00004000,         // Address Length
                    )
            })
            Method (PTS, 1, Serialized)
            {
                If (((Arg0 < 0x06) && (Arg0 > 0x03)))
                {
                    If (!(MORD & 0x10))
                    {
                        MCIP = 0x02
                        IOB0 = MCIN /* \_SB_.TPM_.MCIN */
                    }
                }

                Return (0x00)
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If ((PFPF == 0x01))
                {
                    Return (0x0F)
                }

                Return (0x00)
            }

            Method (HINF, 3, Serialized)
            {
                Switch (ToInteger (Arg1))
                {
                    Case (0x00)
                    {
                        P80L = 0x10
                        Return (Buffer (0x01)
                        {
                             0x03                                             // .
                        })
                    }
                    Case (0x01)
                    {
                        P80L = 0x11
                        Name (TPMV, Package (0x02)
                        {
                            0x01, 
                            Package (0x02)
                            {
                                0x01, 
                                0x20
                            }
                        })
                        If ((_STA () == 0x00))
                        {
                            Return (Package (0x01)
                            {
                                0x00
                            })
                        }

                        Return (TPMV) /* \_SB_.TPM_.HINF.TPMV */
                    }
                    Default
                    {
                        BreakPoint
                    }

                }

                Return (Buffer (0x01)
                {
                     0x00                                             // .
                })
            }

            Name (TPM2, Package (0x02)
            {
                Zero, 
                Zero
            })
            Name (TPM3, Package (0x03)
            {
                Zero, 
                Zero, 
                Zero
            })
            Method (TPPI, 3, Serialized)
            {
                Switch (ToInteger (Arg1))
                {
                    Case (0x00)
                    {
                        P80L = 0x20
                        Return (Buffer (0x02)
                        {
                             0xFF, 0x01                                       // ..
                        })
                    }
                    Case (0x01)
                    {
                        P80L = 0x21
                        Return ("1.3")
                    }
                    Case (0x02)
                    {
                        P80L = 0x22
                        PPRQ = DerefOf (Arg2 [0x00])
                        PPIP = 0x02
                        IOB0 = PPIN /* \_SB_.TPM_.PPIN */
                        Return (FRET) /* \_SB_.TPM_.FRET */
                    }
                    Case (0x03)
                    {
                        P80L = 0x23
                        TPM2 [0x01] = PPRQ /* \_SB_.TPM_.PPRQ */
                        Return (TPM2) /* \_SB_.TPM_.TPM2 */
                    }
                    Case (0x04)
                    {
                        P80L = 0x24
                        Return (0x02)
                    }
                    Case (0x05)
                    {
                        P80L = 0x25
                        PPIP = 0x05
                        IOB0 = PPIN /* \_SB_.TPM_.PPIN */
                        TPM3 [0x01] = LPPR /* \_SB_.TPM_.LPPR */
                        TPM3 [0x02] = PPRP /* \_SB_.TPM_.PPRP */
                        Return (TPM3) /* \_SB_.TPM_.TPM3 */
                    }
                    Case (0x06)
                    {
                        P80L = 0x26
                        Return (0x03)
                    }
                    Case (0x07)
                    {
                        P80L = 0x27
                        PPIP = 0x07
                        PPRQ = DerefOf (Arg2 [0x00])
                        PPRM = 0x00
                        If ((PPRQ == 0x17))
                        {
                            PPRM = DerefOf (Arg2 [0x01])
                        }

                        IOB0 = PPIN /* \_SB_.TPM_.PPIN */
                        Return (FRET) /* \_SB_.TPM_.FRET */
                    }
                    Case (0x08)
                    {
                        P80L = 0x28
                        PPIP = 0x08
                        PPRQ = DerefOf (Arg2 [0x00])
                        IOB0 = PPIN /* \_SB_.TPM_.PPIN */
                        Return (FRET) /* \_SB_.TPM_.FRET */
                    }
                    Default
                    {
                        BreakPoint
                    }

                }

                Return (0x01)
            }

            Method (TMCI, 3, Serialized)
            {
                Switch (ToInteger (Arg1))
                {
                    Case (0x00)
                    {
                        P80L = 0x30
                        Return (Buffer (0x01)
                        {
                             0x03                                             // .
                        })
                    }
                    Case (0x01)
                    {
                        P80L = 0x31
                        MORD = DerefOf (Arg2 [0x00])
                        MCIP = 0x01
                        IOB0 = MCIN /* \_SB_.TPM_.MCIN */
                        Return (MRET) /* \_SB_.TPM_.MRET */
                    }
                    Default
                    {
                        BreakPoint
                    }

                }

                Return (0x01)
            }

            Method (TPMS, 3, Serialized)
            {
                Switch (ToInteger (Arg1))
                {
                    Case (0x00)
                    {
                        Return (Buffer (0x01)
                        {
                             0x03                                             // .
                        })
                    }
                    Case (0x01)
                    {
                        P80L = 0x42
                        Return (0x00)
                    }
                    Default
                    {
                        BreakPoint
                    }

                }

                Return (0x01)
            }

            Method (_DSM, 4, Serialized)  // _DSM: Device-Specific Method
            {
                If ((Arg0 == ToUUID ("cf8e16a5-c1e8-4e25-b712-4f54a96702c8") /* TPM Hardware Information */))
                {
                    P80L = 0x50
                    Return (HINF (Arg1, Arg2, Arg3))
                }

                If ((Arg0 == ToUUID ("3dddfaa6-361b-4eb4-a424-8d10089d1653") /* Physical Presence Interface */))
                {
                    P80L = 0x51
                    Return (TPPI (Arg1, Arg2, Arg3))
                }

                If ((Arg0 == ToUUID ("376054ed-cc13-4675-901c-4756d7f2d45d") /* TPM Memory Clear */))
                {
                    P80L = 0x52
                    Return (TMCI (Arg1, Arg2, Arg3))
                }

                If ((Arg0 == ToUUID ("6bbf6cab-5463-4714-b7cd-f0203c0368d4") /* TPM Start Method */))
                {
                    P80L = 0x53
                    Return (TPMS (Arg1, Arg2, Arg3))
                }

                Return (Buffer (0x01)
                {
                     0x00                                             // .
                })
            }
        }
    }
}

