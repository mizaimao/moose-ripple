/*
 * SPDX-License-Identifier: MIT
 *
 * The driver sources use the second generation of Microsoft's source annotations (_In_,
 * _Out_ and so on). The Windows 7 kit (WDK 7.1) has only the first (__in, __out), so the
 * newer ones are defined away here. They are hints for Microsoft's analysis tools and
 * generate no code. Included before everything else (build.sh, /FI).
 */
#ifndef RIPPLE_SAL2_H
#define RIPPLE_SAL2_H

#define _In_
#define _In_opt_
#define _Inout_
#define _Inout_opt_
#define _Out_
#define _Out_opt_
#define _Outptr_
#define _In_reads_(n)
#define _In_reads_bytes_(n)
#define _In_reads_opt_(n)
#define _Out_writes_(n)
#define _Out_writes_bytes_(n)
#define _Out_writes_opt_(n)
#define _Outptr_result_bytebuffer_(n)
#define _When_(cond, annotation)
#define _IRQL_requires_(irql)
#define _IRQL_requires_max_(irql)
#define _IRQL_requires_min_(irql)
#define _IRQL_requires_same_
#define _IRQL_raises_(irql)
#define _IRQL_saves_
#define _IRQL_restores_
#define _IRQL_saves_global_(kind, param)
#define _IRQL_restores_global_(kind, param)
#define _Function_class_(name)
#define _Use_decl_annotations_
#define _Check_return_
#define _Must_inspect_result_
#define _Success_(expr)
#define _Field_size_(n)
#define _Field_size_bytes_(n)

#endif
