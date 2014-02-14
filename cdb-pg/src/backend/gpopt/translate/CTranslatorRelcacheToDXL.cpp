//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CTranslatorRelcacheToDXL.cpp
//
//	@doc:
//		Class translating relcache entries into DXL objects
//
//	@owner:
//		antovl
//
//	@test:
//
//
//---------------------------------------------------------------------------

#define ALLOW_DatumGetPointer
#define ALLOW_ntohl
#define ALLOW_memset
#define ALLOW_printf
#define ALLOW_offsetof
#define ALLOW_PointerGetDatum
#define ALLOW_Int32GetDatum
#define ALLOW_Int16GetDatum
#define ALLOW_DatumGetInt32
#define ALLOW_MemoryContextAllocImpl
#define ALLOW_pfree
#define ALLOW_MemoryContextFreeImpl
#define ALLOW_CharGetDatum

#include "postgres.h"
#include "md/CMDIdCast.h"
#include "md/CMDIdScCmp.h"

#include "md/CMDCastGPDB.h"
#include "md/CMDScCmpGPDB.h"

#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CTranslatorRelcacheToDXL.h"
#include "gpopt/translate/CTranslatorScalarToDXL.h"
#include "gpopt/mdcache/CMDAccessor.h"

#include "utils/array.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/datum.h"
#include "utils/elog.h"
#include "cdb/cdbhash.h"
#include "access/heapam.h"
#include "catalog/pg_exttable.h"

#include "cdb/cdbpartition.h"
#include "catalog/namespace.h"
#include "catalog/pg_statistic.h"

#undef ALLOW_DatumGetPointer
#undef ALLOW_ntohl
#undef ALLOW_memset
#undef ALLOW_printf
#undef ALLOW_offsetof
#undef ALLOW_PointerGetDatum
#undef ALLOW_Int32GetDatum
#undef ALLOW_Int16GetDatum
#undef ALLOW_DatumGetInt32
#undef ALLOW_CharGetDatum
#undef ALLOW_pfree
#undef ALLOW_MemoryContextAllocImpl
#undef ALLOW_MemoryContextFreeImpl

#define GPDB_COUNT_AGG_OID 2147

#include "gpos/base.h"
#include "gpos/error/CException.h"

#include "exception.h"
#include "dxl/CDXLUtils.h"

#include "dxl/xml/dxltokens.h"

#include "md/CMDTypeBoolGPDB.h"
#include "md/CMDTypeGenericGPDB.h"
#include "md/CMDTypeInt2GPDB.h"
#include "md/CMDTypeInt4GPDB.h"
#include "md/CMDTypeInt8GPDB.h"
#include "md/CMDTypeOidGPDB.h"
#include "md/CMDIndexGPDB.h"
#include "md/CMDPartConstraintGPDB.h"
#include "md/CMDIdRelStats.h"
#include "md/CDXLRelStats.h"
#include "md/CMDIdColStats.h"
#include "md/CDXLColStats.h"

#include "gpopt/base/CUtils.h"

#include "gpopt/gpdbwrappers.h"


using namespace gpdxl;
using namespace gpopt;


static 
const ULONG rgulCmpTypeMappings[][2] = 
{
	{IMDType::EcmptEq, CmptEq},
	{IMDType::EcmptNEq, CmptNEq},
	{IMDType::EcmptL, CmptLT},
	{IMDType::EcmptG, CmptGT},
	{IMDType::EcmptGEq, CmptGEq},
	{IMDType::EcmptLEq, CmptLEq}
};

// TODO: solimm1 - 01/09/2014: remove function properties map when catalog is fixed to include correct properties

// initialization of function properties map
const CTranslatorRelcacheToDXL::SFuncProps CTranslatorRelcacheToDXL::m_rgfp[] =
{
};

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pimdobj
//
//	@doc:
//		Retrieve a metadata object from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::Pimdobj
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	IMDCacheObject *pmdcacheobj = NULL;
	GPOS_ASSERT(NULL != pmda);

	switch(pmdid->Emdidt())
	{
		case IMDId::EmdidGPDB:
			pmdcacheobj = PimdobjGPDB(pmp, pmda, pmdid);
			break;
		
		case IMDId::EmdidRelStats:
			pmdcacheobj = PimdobjRelStats(pmp, pmdid);
			break;
		
		case IMDId::EmdidColStats:
			pmdcacheobj = PimdobjColStats(pmp, pmda, pmdid);
			break;
		
		case IMDId::EmdidCastFunc:
			pmdcacheobj = PimdobjCast(pmp, pmdid);
			break;
		
		case IMDId::EmdidScCmp:
			pmdcacheobj = PmdobjScCmp(pmp, pmdid);
			break;
			
		default:
			break;
	}

	if (NULL == pmdcacheobj)
	{
		// no match found
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	return pmdcacheobj;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjGPDB
//
//	@doc:
//		Retrieve a GPDB metadata object from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjGPDB
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	GPOS_ASSERT(pmdid->Emdidt() == CMDIdGPDB::EmdidGPDB);

	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(0 != oid);

	// find out what type of object this oid stands for

	if (gpdb::FIndexExists(oid))
	{
		return Pmdindex(pmp, pmda, pmdid);
	}

	if (gpdb::FTypeExists(oid))
	{
		return Pmdtype(pmp, pmdid);
	}

	if (gpdb::FRelationExists(oid))
	{
		return Pmdrel(pmp, pmda, pmdid);
	}

	if (gpdb::FOperatorExists(oid))
	{
		return Pmdscop(pmp, pmdid);
	}

	if (gpdb::FAggregateExists(oid))
	{
		return Pmdagg(pmp, pmdid);
	}

	if (gpdb::FFunctionExists(oid))
	{
		return Pmdfunc(pmp, pmdid);
	}

	if (gpdb::FTriggerExists(oid))
	{
		return Pmdtrigger(pmp, pmdid);
	}

	if (gpdb::FCheckConstraintExists(oid))
	{
		return Pmdcheckconstraint(pmp, pmda, pmdid);
	}

	// no match found
	return NULL;

}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdnameRel
//
//	@doc:
//		Return a relation name
//
//---------------------------------------------------------------------------
CMDName *
CTranslatorRelcacheToDXL::PmdnameRel
	(
	IMemoryPool *pmp,
	Relation rel
	)
{
	GPOS_ASSERT(NULL != rel);
	CHAR *szRelName = NameStr(rel->rd_rel->relname);
	CWStringDynamic *pstrRelName = CDXLUtils::PstrFromSz(pmp, szRelName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrRelName);
	delete pstrRelName;
	return pmdname;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpmdidRelIndexes
//
//	@doc:
//		Return the indexes defined on the given relation
//
//---------------------------------------------------------------------------
DrgPmdid *
CTranslatorRelcacheToDXL::PdrgpmdidRelIndexes
	(
	IMemoryPool *pmp,
	Relation rel
	)
{
	GPOS_ASSERT(NULL != rel);
	DrgPmdid *pdrgpmdidIndexes = New(pmp) DrgPmdid(pmp);

	List *plIndexOids = NIL;
	
	if (gpdb::FRelPartIsNone(rel->rd_id))
	{
		// not a partitioned table: obtain indexes directly from the catalog
		plIndexOids = gpdb::PlRelationIndexes(rel);
	}
	else if (gpdb::FRelPartIsRoot(rel->rd_id))
	{
		// root of partitioned table: aggregate index information across different parts
		plIndexOids = PlIndexOidsPartTable(rel);
	}
	else  
	{
		// interior or leaf partition: do not consider indexes
		return pdrgpmdidIndexes;
	}
	
	ListCell *plc = NULL;

	ForEach (plc, plIndexOids)
	{
		OID oidIndex = lfirst_oid(plc);

		// only add supported indexes
		Relation relIndex = gpdb::RelGetRelation(oidIndex);

		GPOS_ASSERT(NULL != relIndex);
		GPOS_ASSERT (NULL != relIndex->rd_indextuple);

		if (FIndexSupported(relIndex))
		{
			CMDIdGPDB *pmdidIndex = New(pmp) CMDIdGPDB(oidIndex);
			pdrgpmdidIndexes->Append(pmdidIndex);
		}

		gpdb::CloseRelation(relIndex);
	}

	return pdrgpmdidIndexes;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PlIndexOidsPartTable
//
//	@doc:
//		Return the index oids of a partitioned table
//
//---------------------------------------------------------------------------
List *
CTranslatorRelcacheToDXL::PlIndexOidsPartTable
	(
	Relation rel
	)
{
	if (!gpdb::FRelPartIsRoot(rel->rd_id))
	{
		// not a partitioned table
		return NIL;
	}

	List *plOids = NIL;
	
	LogicalIndexes *plgidx = gpdb::Plgidx(rel->rd_id);

	if (NULL == plgidx)
	{
		return NIL;
	}
	GPOS_ASSERT(NULL != plgidx);
	GPOS_ASSERT(0 <= plgidx->numLogicalIndexes);
	
	const ULONG ulIndexes = (ULONG) plgidx->numLogicalIndexes;
	for (ULONG ul = 0; ul < ulIndexes; ul++)
	{
		LogicalIndexInfo *pidxinfo = (plgidx->logicalIndexInfo)[ul];
		plOids = gpdb::PlAppendOid(plOids, pidxinfo->logicalIndexOid);
	}
	
	gpdb::GPDBFree(plgidx);
	
	return plOids;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpmdidTriggers
//
//	@doc:
//		Return the triggers defined on the given relation
//
//---------------------------------------------------------------------------
DrgPmdid *
CTranslatorRelcacheToDXL::PdrgpmdidTriggers
	(
	IMemoryPool *pmp,
	Relation rel
	)
{
	GPOS_ASSERT(NULL != rel);
	if (0 < rel->rd_rel->reltriggers && NULL == rel->trigdesc)
	{
		gpdb::BuildRelationTriggers(rel);
		if (NULL == rel->trigdesc)
		{
			rel->rd_rel->reltriggers = 0;
		}
	}

	DrgPmdid *pdrgpmdidTriggers = New(pmp) DrgPmdid(pmp);
	const ULONG ulTriggers = rel->rd_rel->reltriggers;

	for (ULONG ul = 0; ul < ulTriggers; ul++)
	{
		Trigger trigger = rel->trigdesc->triggers[ul];
		OID oidTrigger = trigger.tgoid;
		CMDIdGPDB *pmdidTrigger = New(pmp) CMDIdGPDB(oidTrigger);
		pdrgpmdidTriggers->Append(pmdidTrigger);
	}

	return pdrgpmdidTriggers;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpmdidCheckConstraints
//
//	@doc:
//		Return the check constraints defined on the relation with the given oid
//
//---------------------------------------------------------------------------
DrgPmdid *
CTranslatorRelcacheToDXL::PdrgpmdidCheckConstraints
	(
	IMemoryPool *pmp,
	OID oid
	)
{
	DrgPmdid *pdrgpmdidCheckConstraints = New(pmp) DrgPmdid(pmp);
	List *plOidCheckConstraints = gpdb::PlCheckConstraint(oid);

	ListCell *plcOid = NULL;
	ForEach (plcOid, plOidCheckConstraints)
	{
		OID oidCheckConstraint = lfirst_oid(plcOid);
		GPOS_ASSERT(0 != oidCheckConstraint);
		CMDIdGPDB *pmdidCheckConstraint = New(pmp) CMDIdGPDB(oidCheckConstraint);
		pdrgpmdidCheckConstraints->Append(pmdidCheckConstraint);
	}

	return pdrgpmdidCheckConstraints;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdrel
//
//	@doc:
//		Retrieve a relation from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDRelation *
CTranslatorRelcacheToDXL::Pmdrel
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oid);

	Relation rel = gpdb::RelGetRelation(oid);

	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	// get rel name
	CMDName *pmdname = PmdnameRel(pmp, rel);

	// get storage type
	IMDRelation::Erelstoragetype erelstorage = Erelstorage(rel->rd_rel->relstorage);

	// get relation columns
	DrgPmdcol *pdrgpmdcol = Pdrgpmdcol(pmp, pmda, rel, erelstorage);
	const ULONG ulMaxCols = GPDXL_SYSTEM_COLUMNS + (ULONG) rel->rd_att->natts + 1;
	ULONG *pulAttnoMapping = PulAttnoMapping(pmp, pdrgpmdcol, ulMaxCols);

	// get distribution policy
	GpPolicy *pgppolicy = rel->rd_cdbpolicy;
	IMDRelation::Ereldistrpolicy ereldistribution = Ereldistribution(pgppolicy);

	// get distribution columns
	DrgPul *pdrpulDistrCols = NULL;
	if (IMDRelation::EreldistrHash == ereldistribution)
	{
		pdrpulDistrCols = PdrpulDistrCols(pmp, pgppolicy, pdrgpmdcol, ulMaxCols);
	}

	// collect relation indexes
	DrgPmdid *pdrgpmdidIndexes = PdrgpmdidRelIndexes(pmp, rel);

	// collect relation triggers
	DrgPmdid *pdrgpmdidTriggers = PdrgpmdidTriggers(pmp, rel);

	// get key sets
	BOOL fAddDefaultKeys = FHasSystemColumns(rel->rd_rel->relkind);
	DrgPdrgPul *pdrgpdrgpulKeys = PdrgpdrgpulKeys(pmp, oid, fAddDefaultKeys, pulAttnoMapping);

	// collect all check constraints
	DrgPmdid *pdrgpmdidCheckConstraints = PdrgpmdidCheckConstraints(pmp, oid);

	IMDRelation *pmdrel = NULL;
	pmdid->AddRef();

	BOOL fTemporary = rel->rd_istemp;
	BOOL fHasOids = rel->rd_rel->relhasoids;

	gpdb::CloseRelation(rel);
	delete [] pulAttnoMapping;

	if (IMDRelation::ErelstorageExternal == erelstorage)
	{
		ExtTableEntry *extentry = gpdb::Pexttable(oid);

		// get locations
		DrgPstr *pdrgpstrLocations = New(pmp) DrgPstr(pmp);
		ListCell *plcLocation = NULL;
		ForEach (plcLocation, extentry->locations)
		{
			Value* pvLocation = (Value *)lfirst(plcLocation);
			CWStringDynamic *pstrLocation = CDXLUtils::PstrFromSz(pmp, pvLocation->val.str);
			pdrgpstrLocations->Append(pstrLocation);
		}

		// get format options
		CHAR *szFormatOptions = extentry->fmtopts;
		CWStringDynamic *pstrformatOptions = CDXLUtils::PstrFromSz(pmp, szFormatOptions);

		// get command
		CHAR *szCommand = extentry->command;
		CWStringDynamic *pstrCommand = NULL;
		if (NULL != szCommand)
		{
			pstrCommand = CDXLUtils::PstrFromSz(pmp, szCommand);
		}

		// get format error table id
		IMDId *pmdidFmtErrTbl = NULL;
		if (InvalidOid != extentry->fmterrtbl)
		{
			pmdidFmtErrTbl = New(pmp) CMDIdGPDB(extentry->fmterrtbl);
		}

		pmdrel = New(pmp) CMDRelationExternalGPDB
							(
							pmp,
							pmdid,
							pmdname,
							ereldistribution,
							pdrgpmdcol,
							pdrpulDistrCols,
							pdrgpdrgpulKeys,
							pdrgpmdidIndexes,
							pdrgpmdidTriggers,
							pdrgpmdidCheckConstraints,
							pdrgpstrLocations,
							ErelextFormat(extentry->fmtcode),
							pstrformatOptions,
							pstrCommand,
							extentry->encoding,
							extentry->iswritable,
							extentry->rejectlimit,
							('r' == extentry->rejectlimittype),
							pmdidFmtErrTbl
							);
	}
	else
	{
		// get partition keys
		DrgPul *pdrgpulPartKeys = PdrgpulPartKeys(pmp, oid);

		// get part constraint
		CMDPartConstraintGPDB *pmdpartcnstr = PmdpartcnstrRelation(pmp, pmda, oid, pdrgpmdcol);

		pmdrel = New(pmp) CMDRelationGPDB
							(
							pmp,
							pmdid,
							pmdname,
							fTemporary,
							erelstorage,
							ereldistribution,
							pdrgpmdcol,
							pdrpulDistrCols,
							pdrgpulPartKeys,
							pdrgpdrgpulKeys,
							pdrgpmdidIndexes,
							pdrgpmdidTriggers,
							pdrgpmdidCheckConstraints,
							pmdpartcnstr,
							fHasOids
							);
	}

	return pmdrel;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pdrgpmdcol
//
//	@doc:
//		Get relation columns
//
//---------------------------------------------------------------------------
DrgPmdcol *
CTranslatorRelcacheToDXL::Pdrgpmdcol
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	Relation rel,
	IMDRelation::Erelstoragetype erelstorage
	)
{
	DrgPmdcol *pdrgpmdcol = New(pmp) DrgPmdcol(pmp);

	for (ULONG ul = 0;  ul < (ULONG) rel->rd_att->natts; ul++)
	{
		Form_pg_attribute att = rel->rd_att->attrs[ul];
		CMDName *pmdnameCol = CDXLUtils::PmdnameFromSz(pmp, NameStr(att->attname));
	
		// translate the default column value
		CDXLNode *pdxlnDefault = NULL;
		
		if (!att->attisdropped)
		{
			pdxlnDefault = PdxlnDefaultColumnValue(pmp, pmda, rel->rd_att, att->attnum);
		}

		ULONG ulColLen = ULONG_MAX;
		CMDIdGPDB *pmdidCol = New(pmp) CMDIdGPDB(att->atttypid);
		if ((pmdidCol->FEquals(&CMDIdGPDB::m_mdidBPChar) || pmdidCol->FEquals(&CMDIdGPDB::m_mdidVarChar)) && (VARHDRSZ < att->atttypmod))
		{
			ulColLen = (ULONG) att->atttypmod - VARHDRSZ;
		}

		CMDColumn *pmdcol = New(pmp) CMDColumn
										(
										pmdnameCol,
										att->attnum,
										pmdidCol,
										!att->attnotnull,
										att->attisdropped,
										pdxlnDefault /* default value */,
										ulColLen
										);

		pdrgpmdcol->Append(pmdcol);
	}

	// add system columns
	if (FHasSystemColumns(rel->rd_rel->relkind))
	{
		BOOL fAOTable = IMDRelation::ErelstorageAppendOnlyRows == erelstorage ||
				IMDRelation::ErelstorageAppendOnlyCols == erelstorage ||
				IMDRelation::ErelstorageAppendOnlyParquet == erelstorage;
		AddSystemColumns(pmp, pdrgpmdcol, rel->rd_att->tdhasoid, fAOTable);
	}

	return pdrgpmdcol;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdxlnDefaultColumnValue
//
//	@doc:
//		Return the dxl representation of column's default value
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorRelcacheToDXL::PdxlnDefaultColumnValue
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	TupleDesc rd_att,
	AttrNumber attno
	)
{
	GPOS_ASSERT(attno > 0);

	Node *pnode = NULL;

	// Scan to see if relation has a default for this column
	if (NULL != rd_att->constr && 0 < rd_att->constr->num_defval)
	{
		AttrDefault *defval = rd_att->constr->defval;
		INT	iNumDef = rd_att->constr->num_defval;

		GPOS_ASSERT(NULL != defval);
		for (ULONG ulCounter = 0; ulCounter < (ULONG) iNumDef; ulCounter++)
		{
			if (attno == defval[ulCounter].adnum)
			{
				// found it, convert string representation to node tree.
				pnode = gpdb::Pnode(defval[ulCounter].adbin);
				break;
			}
		}
	}

	if (NULL == pnode)
	{
		// get the default value for the type
		Form_pg_attribute att_tup = rd_att->attrs[attno - 1];
		Oid	oidAtttype = att_tup->atttypid;
		pnode = gpdb::PnodeTypeDefault(oidAtttype);
	}

	if (NULL == pnode)
	{
		return NULL;
	}

	// translate the default value expression
	CTranslatorScalarToDXL sctranslator
							(
							pmp,
							pmda,
							NULL, /* pulidgtorCol */
							NULL, /* pulidgtorCTE */
							0, /* ulQueryLevel */
							true, /* m_fQuery */
							NULL, /* m_pplstmt */
							NULL, /* m_pmappv */
							NULL, /* phmulCTEEntries */
							NULL /* pdrgpdxlnCTE */
							);

	return sctranslator.PdxlnScOpFromExpr
							(
							(Expr *) pnode,
							NULL /* pmapvarcolid --- subquery or external variable are not supported in default expression */
							);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::ErelextFormat
//
//	@doc:
//		Return the format type of an external relation
//
//---------------------------------------------------------------------------
IMDRelationExternal::Erelextformattype
CTranslatorRelcacheToDXL::ErelextFormat
	(
	char fmtcode
	)
{
	switch (fmtcode)
	{
		case 'c':
			return IMDRelationExternal::ErelextformatCSV;
		case 't':
			return IMDRelationExternal::ErelextformatText;
		case 'b':
			return IMDRelationExternal::ErelextformatCustom;
		default:
			GPOS_ASSERT(!"Unrecognized external table format");
			return IMDRelationExternal::ErelextformatSentinel;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Ereldistribution
//
//	@doc:
//		Return the distribution policy of the relation
//
//---------------------------------------------------------------------------
IMDRelation::Ereldistrpolicy
CTranslatorRelcacheToDXL::Ereldistribution
	(
	GpPolicy *pgppolicy
	)
{
	if (NULL == pgppolicy)
	{
		return IMDRelation::EreldistrMasterOnly;
	}

	if (POLICYTYPE_PARTITIONED == pgppolicy->ptype)
	{
		if (0 == pgppolicy->nattrs)
		{
			return IMDRelation::EreldistrRandom;
		}

		return IMDRelation::EreldistrHash;
	}

	if (POLICYTYPE_ENTRY == pgppolicy->ptype)
	{
		return IMDRelation::EreldistrMasterOnly;
	}

	GPOS_ASSERT(!"Unrecognized distribution policy");
	return IMDRelation::EreldistrSentinel;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrpulDistrCols
//
//	@doc:
//		Get distribution columns
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorRelcacheToDXL::PdrpulDistrCols
	(
	IMemoryPool *pmp,
	GpPolicy *pgppolicy,
	DrgPmdcol *pdrgpmdcol,
	ULONG ulSize
	)
{
	ULONG *pul = New(pmp) ULONG[ulSize];

	for (ULONG ul = 0;  ul < pdrgpmdcol->UlLength(); ul++)
	{
		const IMDColumn *pmdcol = (*pdrgpmdcol)[ul];
		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		pul[ulIndex] = ul;
	}

	DrgPul *pdrpulDistrCols = New(pmp) DrgPul(pmp);

	for (ULONG ul = 0; ul < (ULONG) pgppolicy->nattrs; ul++)
	{
		AttrNumber attno = pgppolicy->attrs[ul];
		pdrpulDistrCols->Append(New(pmp) ULONG(UlPosition(attno, pul)));
	}

	delete[] pul;
	return pdrpulDistrCols;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::AddSystemColumns
//
//	@doc:
//		Adding system columns (oid, tid, xmin, etc) in table descriptors
//
//---------------------------------------------------------------------------
void
CTranslatorRelcacheToDXL::AddSystemColumns
	(
	IMemoryPool *pmp,
	DrgPmdcol *pdrgpmdcol,
	BOOL fHasOid,
	BOOL fAOTable
	)
{
	for (INT i= SelfItemPointerAttributeNumber; i > FirstLowInvalidHeapAttributeNumber; i--)
	{
		AttrNumber attno = AttrNumber(i);
		GPOS_ASSERT(0 != attno);

		if (ObjectIdAttributeNumber == i && !fHasOid)
		{
			continue;
		}
		
		if (FTransactionVisibilityAttribute(i) && fAOTable)
		{
			// skip transaction attrbutes like xmin, xmax, cmin, cmax for AO tables
			continue;
		}
		
		// get system name for that attribute
		const CWStringConst *pstrSysColName = CTranslatorUtils::PstrSystemColName(attno);
		GPOS_ASSERT(NULL != pstrSysColName);

		// copy string into column name
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pstrSysColName);

		CMDColumn *pmdcol = New(pmp) CMDColumn
										(
										pmdnameCol, 
										attno, 
										CTranslatorUtils::PmdidSystemColType(pmp, attno), 
										false,	// fNullable
										false,	// fDropped
										NULL	// default value
										);

		pdrgpmdcol->Append(pmdcol);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::FTransactionVisibilityAttribute
//
//	@doc:
//		Check if attribute number is one of the system attributes related to 
//		transaction visibility such as xmin, xmax, cmin, cmax
//
//---------------------------------------------------------------------------
BOOL
CTranslatorRelcacheToDXL::FTransactionVisibilityAttribute
	(
	INT iAttNo
	)
{
	return iAttNo == MinTransactionIdAttributeNumber || iAttNo == MaxTransactionIdAttributeNumber || 
			iAttNo == MinCommandIdAttributeNumber || iAttNo == MaxCommandIdAttributeNumber;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdindex
//
//	@doc:
//		Retrieve an index from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDIndex *
CTranslatorRelcacheToDXL::Pmdindex
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdidIndex
	)
{
	OID oidIndex = CMDIdGPDB::PmdidConvert(pmdidIndex)->OidObjectId();
	GPOS_ASSERT(0 != oidIndex);
	Relation relIndex = gpdb::RelGetRelation(oidIndex);

	if (NULL == relIndex)
	{
		 GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdidIndex->Wsz());
	}

	if (!FIndexSupported(relIndex))
	{
		gpdb::CloseRelation(relIndex);
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported, GPOS_WSZ_LIT("Index type"));
	}
	
	Form_pg_index pgIndex = relIndex->rd_index;
	GPOS_ASSERT (NULL != pgIndex);
	
	OID oidRel = pgIndex->indrelid;
	
	if (gpdb::FLeafPartition(oidRel))
	{
		oidRel = gpdb::OidRootPartition(oidRel);
	}
	
	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(oidRel);
	
	const IMDRelation *pmdrel = pmda->Pmdrel(pmdidRel);

	if (pmdrel->FPartitioned())
	{
		LogicalIndexes *plgidx = gpdb::Plgidx(oidRel);
		GPOS_ASSERT(NULL != plgidx);
		
		IMDIndex *pmdindex = PmdindexPartTable(pmp, pmda, pmdidIndex, pmdrel, plgidx);
		
		// cleanup
		pmdidRel->Release();

		gpdb::CloseRelation(relIndex);
		gpdb::GPDBFree(plgidx);
		
		return pmdindex;
	}

	// get the index name
	CHAR *szIndexName = NameStr(relIndex->rd_rel->relname);
	CWStringDynamic *pstrName = CDXLUtils::PstrFromSz(pmp, szIndexName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrName);
	delete pstrName;

	Relation relTable = gpdb::RelGetRelation(CMDIdGPDB::PmdidConvert(pmdrel->Pmdid())->OidObjectId());
	ULONG ulRgSize = GPDXL_SYSTEM_COLUMNS + (ULONG) relTable->rd_att->natts + 1;

	ULONG *pul = PulAttnoPositionMap(pmp, pmdrel, ulRgSize);

	DrgPul *pdrgpulIncludeCols = PdrgpulIndexIncludedColumns(pmp, pmdrel);

	// extract the position of the key columns
	DrgPul *pdrgpulKeyCols = New(pmp) DrgPul(pmp);
	ULONG ulKeys = pgIndex->indnatts;
	for (ULONG ul = 0; ul < ulKeys; ul++)
	{
		INT iAttno = pgIndex->indkey.values[ul];
		GPOS_ASSERT(0 != iAttno && "Index expressions not supported");

		pdrgpulKeyCols->Append(New(pmp) ULONG(UlPosition(iAttno, pul)));
	}

	pmdidIndex->AddRef();	
	CMDIndexGPDB *pmdindex = New(pmp) CMDIndexGPDB
										(
										pmp,
										pmdidIndex,
										pmdname,
										New(pmp) CMDIdGPDB(pgIndex->indrelid),
										pgIndex->indisclustered,
										false, // fPartial
										pdrgpulKeyCols,
										pdrgpulIncludeCols,
										NULL // pmdpartcnstr
										);

	delete [] pul;
	pmdidRel->Release();
	gpdb::CloseRelation(relIndex);
	gpdb::CloseRelation(relTable);
	return pmdindex;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdindexPartTable
//
//	@doc:
//		Retrieve an index over a partitioned table from the relcache given its 
//		mdid
//
//---------------------------------------------------------------------------
IMDIndex *
CTranslatorRelcacheToDXL::PmdindexPartTable
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdidIndex,
	const IMDRelation *pmdrel,
	LogicalIndexes *plind
	)
{
	GPOS_ASSERT(NULL != plind);
	GPOS_ASSERT(0 < plind->numLogicalIndexes);
	
	OID oid = CMDIdGPDB::PmdidConvert(pmdidIndex)->OidObjectId();
	
	LogicalIndexInfo *pidxinfo = PidxinfoLookup(plind, oid);
	if (NULL == pidxinfo)
	{
		 GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdidIndex->Wsz());
	}
	
	return PmdindexPartTable(pmp, pmda, pidxinfo, pmdidIndex, pmdrel);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PidxinfoLookup
//
//	@doc:
//		Lookup an index given its id from the logical indexes structure
//
//---------------------------------------------------------------------------
LogicalIndexInfo *
CTranslatorRelcacheToDXL::PidxinfoLookup
	(
	LogicalIndexes *plind, 
	OID oid
	)
{
	GPOS_ASSERT(NULL != plind && 0 <= plind->numLogicalIndexes);
	
	const ULONG ulIndexes = plind->numLogicalIndexes;
	
	for (ULONG ul = 0; ul < ulIndexes; ul++)
	{
		LogicalIndexInfo *pidxinfo = (plind->logicalIndexInfo)[ul];
		
		if (oid == pidxinfo->logicalIndexOid)
		{
			return pidxinfo;
		}
	}
	
	return NULL;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdindexPartTable
//
//	@doc:
//		Construct an MD cache index object given its logical index representation
//
//---------------------------------------------------------------------------
IMDIndex *
CTranslatorRelcacheToDXL::PmdindexPartTable
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	LogicalIndexInfo *pidxinfo,
	IMDId *pmdidIndex,
	const IMDRelation *pmdrel
	)
{
	OID oidIndex = pidxinfo->logicalIndexOid;
	
	Relation relIndex = gpdb::RelGetRelation(oidIndex);

	if (NULL == relIndex)
	{
		gpdb::CloseRelation(relIndex);
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdidIndex->Wsz());
	}

	if (!FIndexSupported(relIndex))
	{
		gpdb::CloseRelation(relIndex);
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported, GPOS_WSZ_LIT("Index type"));
	}
	
	// get the index name
	GPOS_ASSERT (NULL != relIndex->rd_index);
	Form_pg_index pgIndex = relIndex->rd_index;
	
	CHAR *szIndexName = NameStr(relIndex->rd_rel->relname);
	CMDName *pmdname = CDXLUtils::PmdnameFromSz(pmp, szIndexName);

	OID oidRel = CMDIdGPDB::PmdidConvert(pmdrel->Pmdid())->OidObjectId();
	Relation relTable = gpdb::RelGetRelation(oidRel);
	ULONG ulRgSize = GPDXL_SYSTEM_COLUMNS + (ULONG) relTable->rd_att->natts + 1;

	ULONG *pulAttrMap = PulAttnoPositionMap(pmp, pmdrel, ulRgSize);

	DrgPul *pdrgpulIncludeCols = PdrgpulIndexIncludedColumns(pmp, pmdrel);

	// extract the position of the key columns
	DrgPul *pdrgpulKeyCols = New(pmp) DrgPul(pmp);
	const ULONG ulKeys = pidxinfo->nColumns;
	for (ULONG ul = 0; ul < ulKeys; ul++)
	{
		INT iAttno = pidxinfo->indexKeys[ul];
		GPOS_ASSERT(0 != iAttno && "Index expressions not supported");

		pdrgpulKeyCols->Append(New(pmp) ULONG(UlPosition(iAttno, pulAttrMap)));
	}
	
	Node *pnodePartCnstr = pidxinfo->partCons;
	List *plDefaultLevels = pidxinfo->defaultLevels;
	
	// get relation constraints
	BOOL fDefaultPartitionRel = false;
	Node *pnodePartCnstrRel = gpdb::PnodePartConstraintRel(oidRel, &fDefaultPartitionRel);
	
	BOOL fUnbounded = (NULL == pnodePartCnstr) && (NIL == plDefaultLevels) && fDefaultPartitionRel;
	BOOL fDefaultPartition = fUnbounded || FDefaultPartition(plDefaultLevels, 0 /*ulLevel*/);
	BOOL fPartial = (NULL != pnodePartCnstr || NIL != plDefaultLevels);

	if (NULL == pnodePartCnstr)
	{
		if (NIL == plDefaultLevels)
		{
			// NULL part constraints means all non-default partitions -> get constraint from the part table
			pnodePartCnstr = pnodePartCnstrRel;
		}
		else
		{
			GPOS_ASSERT(fDefaultPartition);
			pnodePartCnstr = gpdb::PnodeMakeBoolConst(false /*value*/, false /*isull*/);
		}
	}
		
	CMDPartConstraintGPDB *pmdpartcnstr = PmdpartcnstrIndex(pmp, pmda, pmdrel, pnodePartCnstr, fDefaultPartition, fUnbounded);
	
	pmdrel->Pmdid()->AddRef();
	pmdidIndex->AddRef();
	
	CMDIndexGPDB *pmdindex = New(pmp) CMDIndexGPDB
										(
										pmp,
										pmdidIndex,
										pmdname,
										pmdrel->Pmdid(),
										pgIndex->indisclustered,
										fPartial,
										pdrgpulKeyCols,
										pdrgpulIncludeCols,
										pmdpartcnstr
										);
	
	gpdb::CloseRelation(relIndex);
	gpdb::CloseRelation(relTable);

	delete [] pulAttrMap;
	
	return pmdindex;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::FDefaultPartition
//
//	@doc:
//		Check whether the default partition at level one is included
//
//---------------------------------------------------------------------------
BOOL
CTranslatorRelcacheToDXL::FDefaultPartition
	(
	List *plDefaultLevels,
	ULONG ulLevel
	)
{
	if (NIL == plDefaultLevels)
	{
		return false;
	}
	
	ListCell *plc = NULL;
	ForEach (plc, plDefaultLevels)
	{
		ULONG ulDefaultLevel = (ULONG) lfirst_int(plc);
		if (ulLevel == ulDefaultLevel)
		{
			return true;
		}
	}
	
	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpulIndexIncludedColumns
//
//	@doc:
//		Compute the included colunms in an index
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorRelcacheToDXL::PdrgpulIndexIncludedColumns
	(
	IMemoryPool *pmp,
	const IMDRelation *pmdrel
	)
{
	// TODO: raghav, 3/19/2012; currently we assume that all the columns
	// in the table are available from the index.

	DrgPul *pdrgpulIncludeCols = New(pmp) DrgPul(pmp);
	const ULONG ulIncludedCols = pmdrel->UlColumns();
	for (ULONG ul = 0;  ul < ulIncludedCols; ul++)
	{
		if (!pmdrel->Pmdcol(ul)->FDropped())
		{
			pdrgpulIncludeCols->Append(New(pmp) ULONG(ul));
		}
	}
	
	return pdrgpulIncludeCols;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::UlPosition
//
//	@doc:
//		Return the position of a given attribute
//
//---------------------------------------------------------------------------
ULONG
CTranslatorRelcacheToDXL::UlPosition
	(
	INT iAttno,
	ULONG *pul
	)
{
	ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
	ULONG ulPos = pul[ulIndex];
	GPOS_ASSERT(ULONG_MAX != ulPos);

	return ulPos;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PulAttnoPositionMap
//
//	@doc:
//		Populate the attribute to position mapping
//
//---------------------------------------------------------------------------
ULONG *
CTranslatorRelcacheToDXL::PulAttnoPositionMap
	(
	IMemoryPool *pmp,
	const IMDRelation *pmdrel,
	ULONG ulSize
	)
{
	GPOS_ASSERT(NULL != pmdrel);
	const ULONG ulIncludedCols = pmdrel->UlColumns();

	GPOS_ASSERT(ulIncludedCols <= ulSize);
	ULONG *pul = New(pmp) ULONG[ulSize];

	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		pul[ul] = ULONG_MAX;
	}

	for (ULONG ul = 0;  ul < ulIncludedCols; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);

		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		GPOS_ASSERT(ulSize > ulIndex);
		pul[ulIndex] = ul;
	}

	return pul;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdtype
//
//	@doc:
//		Retrieve a type from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDType *
CTranslatorRelcacheToDXL::Pmdtype
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidType = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	GPOS_ASSERT(InvalidOid != oidType);
	
	// check for supported base types
	switch (oidType)
	{
		case GPDB_INT2_OID:
			return New(pmp) CMDTypeInt2GPDB(pmp);

		case GPDB_INT4_OID:
			return New(pmp) CMDTypeInt4GPDB(pmp);

		case GPDB_INT8_OID:
			return New(pmp) CMDTypeInt8GPDB(pmp);

		case GPDB_BOOL:
			return New(pmp) CMDTypeBoolGPDB(pmp);

		case GPDB_OID_OID:
			return New(pmp) CMDTypeOidGPDB(pmp);
	}

	// continue to construct a generic type
	INT iFlags = TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				 TYPECACHE_CMP_PROC | TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO | TYPECACHE_TUPDESC;

	TypeCacheEntry *ptce = gpdb::PtceLookup(oidType, iFlags);

	// get type name
	CMDName *pmdname = PmdnameType(pmp, pmdid);

	BOOL fFixedLength = false;
	ULONG ulLength = 0;

	if (0 < ptce->typlen)
	{
		fFixedLength = true;
		ulLength = ptce->typlen;
	}

	BOOL fByValue = ptce->typbyval;

	// collect ids of different comparison operators for types
	CMDIdGPDB *pmdidOpEq = New(pmp) CMDIdGPDB(ptce->eq_opr);
	CMDIdGPDB *pmdidOpNEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->eq_opr));
	CMDIdGPDB *pmdidOpLT = New(pmp) CMDIdGPDB(ptce->lt_opr);
	CMDIdGPDB *pmdidOpLEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->gt_opr));
	CMDIdGPDB *pmdidOpGT = New(pmp) CMDIdGPDB(ptce->gt_opr);
	CMDIdGPDB *pmdidOpGEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->lt_opr));
	CMDIdGPDB *pmdidOpComp = New(pmp) CMDIdGPDB(ptce->cmp_proc);
	BOOL fHashable = gpdb::FOpHashJoinable(ptce->eq_opr);
	BOOL fComposite = gpdb::FCompositeType(oidType);

	// get standard aggregates
	CMDIdGPDB *pmdidMin = New(pmp) CMDIdGPDB(gpdb::OidAggregate("min", oidType));
	CMDIdGPDB *pmdidMax = New(pmp) CMDIdGPDB(gpdb::OidAggregate("max", oidType));
	CMDIdGPDB *pmdidAvg = New(pmp) CMDIdGPDB(gpdb::OidAggregate("avg", oidType));
	CMDIdGPDB *pmdidSum = New(pmp) CMDIdGPDB(gpdb::OidAggregate("sum", oidType));
	
	// count aggregate is the same for all types
	CMDIdGPDB *pmdidCount = New(pmp) CMDIdGPDB(GPDB_COUNT_AGG_OID);
	
	// check if type is composite
	CMDIdGPDB *pmdidTypeRelid = NULL;
	if (fComposite)
	{
		pmdidTypeRelid = New(pmp) CMDIdGPDB(gpdb::OidTypeRelid(oidType));
	}

	// get array type mdid
	CMDIdGPDB *pmdidTypeArray = New(pmp) CMDIdGPDB(gpdb::OidArrayType(oidType));

	BOOL fRedistributable = gpdb::FGreenplumDbHashable(oidType);

	pmdid->AddRef();

	return New(pmp) CMDTypeGenericGPDB
						 (
						 pmp,
						 pmdid,
						 pmdname,
						 fRedistributable,
						 fFixedLength,
						 ulLength,
						 fByValue,
						 pmdidOpEq,
						 pmdidOpNEq,
						 pmdidOpLT,
						 pmdidOpLEq,
						 pmdidOpGT,
						 pmdidOpGEq,
						 pmdidOpComp,
						 pmdidMin,
						 pmdidMax,
						 pmdidAvg,
						 pmdidSum,
						 pmdidCount,
						 fHashable,
						 fComposite,
						 pmdidTypeRelid,
						 pmdidTypeArray,
						 ptce->typlen
						 );
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdscop
//
//	@doc:
//		Retrieve a scalar operator from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDScalarOpGPDB *
CTranslatorRelcacheToDXL::Pmdscop
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidOp = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidOp);

	// get operator name
	CHAR *szName = gpdb::SzOpName(oidOp);

	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CMDName *pmdname = CDXLUtils::PmdnameFromSz(pmp, szName);
	
	OID oidLeft = InvalidOid;
	OID oidRight = InvalidOid;

	// get operator argument types
	gpdb::GetOpInputTypes(oidOp, &oidLeft, &oidRight);

	CMDIdGPDB *pmdidTypeLeft = NULL;
	CMDIdGPDB *pmdidTypeRight = NULL;

	if (InvalidOid != oidLeft)
	{
		pmdidTypeLeft = New(pmp) CMDIdGPDB(oidLeft);
	}

	if (InvalidOid != oidRight)
	{
		pmdidTypeRight = New(pmp) CMDIdGPDB(oidRight);
	}

	// get comparison type
	CmpType cmpt = (CmpType) gpdb::UlCmpt(oidOp, oidLeft, oidRight);
	IMDType::ECmpType ecmpt = Ecmpt(cmpt);
	
	// get func oid
	OID oidFunc = gpdb::OidOpFunc(oidOp);
	GPOS_ASSERT(InvalidOid != oidFunc);

	CMDIdGPDB *pmdidFunc = New(pmp) CMDIdGPDB(oidFunc);

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidFunc);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);

	// get commutator and inverse
	CMDIdGPDB *pmdidOpCommute = NULL;

	OID oidCommute = gpdb::OidCommutatorOp(oidOp);

	if(InvalidOid != oidCommute)
	{
		pmdidOpCommute = New(pmp) CMDIdGPDB(oidCommute);
	}

	CMDIdGPDB *pmdidOpInverse = NULL;

	OID oidInverse = gpdb::OidInverseOp(oidOp);

	if(InvalidOid != oidInverse)
	{
		pmdidOpInverse = New(pmp) CMDIdGPDB(oidInverse);
	}

	pmdid->AddRef();
	CMDScalarOpGPDB *pmdscop = New(pmp) CMDScalarOpGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeLeft,
											pmdidTypeRight,
											pmdidTypeResult,
											pmdidFunc,
											pmdidOpCommute,
											pmdidOpInverse,
											ecmpt
											);
	return pmdscop;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::LookupFuncProps
//
//	@doc:
//		Lookup function properties
//
//---------------------------------------------------------------------------
void
CTranslatorRelcacheToDXL::LookupFuncProps
	(
	OID oidFunc,
	IMDFunction::EFuncStbl *pefs, // output: function stability
	IMDFunction::EFuncDataAcc *pefda, // output: function datya access
	BOOL *fStrict, // output: is function strict?
	BOOL *fReturnsSet // output: does function return set?
	)
{
	GPOS_ASSERT(NULL != pefs);
	GPOS_ASSERT(NULL != pefda);
	GPOS_ASSERT(NULL != fStrict);
	GPOS_ASSERT(NULL != fReturnsSet);

	// lookup function properties first n the local map
	const ULONG ulSize = GPOS_ARRAY_SIZE(m_rgfp);
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		if (m_rgfp[ul].Oid() == oidFunc)
		{
			*pefs = m_rgfp[ul].Efs();
			*pefda = m_rgfp[ul].Efda();
			*fStrict = m_rgfp[ul].FStrict();
			*fReturnsSet = m_rgfp[ul].FReturnsSet();

			return;
		}
	}

	CHAR cFuncStability = gpdb::CFuncStability(oidFunc);
	*pefs = EFuncStability(cFuncStability);

	CHAR cFuncDataAccess = gpdb::CFuncDataAccess(oidFunc);
	*pefda = EFuncDataAccess(cFuncDataAccess);

	*fReturnsSet = gpdb::FFuncRetset(oidFunc);
	*fStrict = gpdb::FFuncStrict(oidFunc);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdfunc
//
//	@doc:
//		Retrieve a function from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDFunctionGPDB *
CTranslatorRelcacheToDXL::Pmdfunc
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidFunc = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidFunc);

	// get func name
	CHAR *szName = gpdb::SzFuncName(oidFunc);

	if (NULL == szName)
	{

		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrFuncName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrFuncName);

	// CMDName ctor created a copy of the string
	delete pstrFuncName;

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidFunc);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);

	// get output argument types if any
	List *plOutArgTypes = gpdb::PlFuncOutputArgTypes(oidFunc);

	DrgPmdid *pdrgpmdidArgTypes = NULL;
	if (NULL != plOutArgTypes)
	{
		ListCell *plc = NULL;
		pdrgpmdidArgTypes = New(pmp) DrgPmdid(pmp);

		ForEach (plc, plOutArgTypes)
		{
			OID oidArgType = lfirst_oid(plc);
			GPOS_ASSERT(InvalidOid != oidArgType);
			CMDIdGPDB *pmdidArgType = New(pmp) CMDIdGPDB(oidArgType);
			pdrgpmdidArgTypes->Append(pmdidArgType);
		}

		gpdb::GPDBFree(plOutArgTypes);
	}

	IMDFunction::EFuncStbl efs = IMDFunction::EfsImmutable;
	IMDFunction::EFuncDataAcc efda = IMDFunction::EfdaNoSQL;
	BOOL fStrict = true;
	BOOL fReturnsSet = true;
	LookupFuncProps(oidFunc, &efs, &efda, &fStrict, &fReturnsSet);

	pmdid->AddRef();
	CMDFunctionGPDB *pmdfunc = New(pmp) CMDFunctionGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeResult,
											pdrgpmdidArgTypes,
											fReturnsSet,
											efs,
											efda,
											fStrict
											);

	return pmdfunc;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdagg
//
//	@doc:
//		Retrieve an aggregate from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDAggregateGPDB *
CTranslatorRelcacheToDXL::Pmdagg
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidAgg = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidAgg);

	// get agg name
	CHAR *szName = gpdb::SzFuncName(oidAgg);

	if (NULL == szName)
	{

		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrAggName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrAggName);

	// CMDName ctor created a copy of the string
	delete pstrAggName;

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidAgg);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);
	IMDId *pmdidTypeIntermediate = PmdidAggIntermediateResultType(pmp, pmdid);

	pmdid->AddRef();
	
	BOOL fOrdered = gpdb::FOrderedAgg(oidAgg);
	
	// GPDB does not support splitting of ordered aggs and aggs without a
	// preliminary function
	BOOL fSplittable = !fOrdered && gpdb::FAggHasPrelimFunc(oidAgg);
	
	CMDAggregateGPDB *pmdagg = New(pmp) CMDAggregateGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeResult,
											pmdidTypeIntermediate,
											fOrdered,
											fSplittable
											);
	return pmdagg;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdtrigger
//
//	@doc:
//		Retrieve a trigger from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDTriggerGPDB *
CTranslatorRelcacheToDXL::Pmdtrigger
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidTrigger = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidTrigger);

	// get trigger name
	CHAR *szName = gpdb::SzTriggerName(oidTrigger);

	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrTriggerName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrTriggerName);
	delete pstrTriggerName;

	// get relation oid
	OID oidRel = gpdb::OidTriggerRelid(oidTrigger);
	GPOS_ASSERT(InvalidOid != oidRel);
	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(oidRel);

	// get function oid
	OID oidFunc = gpdb::OidTriggerFuncid(oidTrigger);
	GPOS_ASSERT(InvalidOid != oidFunc);
	CMDIdGPDB *pmdidFunc = New(pmp) CMDIdGPDB(oidFunc);

	// get type
	INT iType = gpdb::ITriggerType(oidTrigger);

	// is trigger enabled
	BOOL fEnabled = gpdb::FTriggerEnabled(oidTrigger);

	pmdid->AddRef();
	CMDTriggerGPDB *pmdtrigger = New(pmp) CMDTriggerGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidRel,
											pmdidFunc,
											iType,
											fEnabled
											);
	return pmdtrigger;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdcheckconstraint
//
//	@doc:
//		Retrieve a check constraint from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDCheckConstraintGPDB *
CTranslatorRelcacheToDXL::Pmdcheckconstraint
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	OID oidCheckConstraint = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	GPOS_ASSERT(InvalidOid != oidCheckConstraint);

	// get name of the check constraint
	CHAR *szName = gpdb::SzCheckConstraintName(oidCheckConstraint);
	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}
	CWStringDynamic *pstrCheckConstraintName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrCheckConstraintName);
	delete pstrCheckConstraintName;

	// get relation oid associated with the check constraint
	OID oidRel = gpdb::OidCheckConstraintRelid(oidCheckConstraint);
	GPOS_ASSERT(InvalidOid != oidRel);
	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(oidRel);

	// translate the check constraint expression
	Node *pnode = gpdb::PnodeCheckConstraint(oidCheckConstraint);
	GPOS_ASSERT(NULL != pnode);

	CTranslatorScalarToDXL sctranslator
							(
							pmp,
							pmda,
							NULL, /* pulidgtorCol */
							NULL, /* pulidgtorCTE */
							0, /* ulQueryLevel */
							true, /* m_fQuery */
							NULL, /* m_pplstmt */
							NULL, /* m_pmappv */
							NULL, /* phmulCTEEntries */
							NULL /* pdrgpdxlnCTE */
							);

	// generate a mock mapping between var to column information
	CMappingVarColId *pmapvarcolid = New(pmp) CMappingVarColId(pmp);
	DrgPdxlcd *pdrgpdxlcd = New(pmp) DrgPdxlcd(pmp);
	const IMDRelation *pmdrel = pmda->Pmdrel(pmdidRel);
	const ULONG ulLen = pmdrel->UlColumns();
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		CMDIdGPDB *pmdidColType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType());
		pmdidColType->AddRef();

		// create a column descriptor for the column
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdnameCol,
										ul + 1 /*ulColId*/,
										pmdcol->IAttno(),
										pmdidColType,
										false /* fColDropped */
										);
		pdrgpdxlcd->Append(pdxlcd);
	}
	pmapvarcolid->LoadColumns(0 /*ulQueryLevel */, 1 /* rteIndex */, pdrgpdxlcd);

	// translate the check constraint expression
	CDXLNode *pdxlnScalar = sctranslator.PdxlnScOpFromExpr((Expr *) pnode, pmapvarcolid);

	// cleanup
	pdrgpdxlcd->Release();
	delete pmapvarcolid;

	pmdid->AddRef();

	return New(pmp) CMDCheckConstraintGPDB
						(
						pmp,
						pmdid,
						pmdname,
						pmdidRel,
						pdxlnScalar
						);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdnameType
//
//	@doc:
//		Retrieve a type's name from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDName *
CTranslatorRelcacheToDXL::PmdnameType
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidType = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidType);

	CHAR *szTypeName = gpdb::SzTypeName(oidType);
	GPOS_ASSERT(NULL != szTypeName);

	CWStringDynamic *pstrName = CDXLUtils::PstrFromSz(pmp, szTypeName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrName);

	// cleanup
	delete pstrName;
	return pmdname;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::EFuncStability
//
//	@doc:
//		Get function stability property from the GPDB character representation
//
//---------------------------------------------------------------------------
CMDFunctionGPDB::EFuncStbl
CTranslatorRelcacheToDXL::EFuncStability
	(
	CHAR c
	)
{
	CMDFunctionGPDB::EFuncStbl efuncstbl = CMDFunctionGPDB::EfsSentinel;

	switch (c)
	{
		case 's':
			efuncstbl = CMDFunctionGPDB::EfsStable;
			break;
		case 'i':
			efuncstbl = CMDFunctionGPDB::EfsImmutable;
			break;
		case 'v':
			efuncstbl = CMDFunctionGPDB::EfsVolatile;
			break;
		default:
			GPOS_ASSERT(!"Invalid stability property");
	}

	return efuncstbl;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::EFuncDataAccess
//
//	@doc:
//		Get function data access property from the GPDB character representation
//
//---------------------------------------------------------------------------
CMDFunctionGPDB::EFuncDataAcc
CTranslatorRelcacheToDXL::EFuncDataAccess
	(
	CHAR c
	)
{
	CMDFunctionGPDB::EFuncDataAcc efda = CMDFunctionGPDB::EfdaSentinel;

	switch (c)
	{
		case 'n':
			efda = CMDFunctionGPDB::EfdaNoSQL;
			break;
		case 'c':
			efda = CMDFunctionGPDB::EfdaContainsSQL;
			break;
		case 'r':
			efda = CMDFunctionGPDB::EfdaReadsSQLData;
			break;
		case 'm':
			efda = CMDFunctionGPDB::EfdaModifiesSQLData;
			break;
		default:
			GPOS_ASSERT(!"Invalid data access property");
	}

	return efda;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdidAggIntermediateResultType
//
//	@doc:
//		Retrieve the type id of an aggregate's intermediate results
//
//---------------------------------------------------------------------------
IMDId *
CTranslatorRelcacheToDXL::PmdidAggIntermediateResultType
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidAgg = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidAgg);

	OID oidTypeIntermediateResult = gpdb::OidAggIntermediateResultType(oidAgg);
	return New(pmp) CMDIdGPDB(oidTypeIntermediateResult);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdidFunc
//
//	@doc:
//		Return the id of the function with the given name
//
//---------------------------------------------------------------------------
IMDId *
CTranslatorRelcacheToDXL::PmdidFunc
	(
	IMemoryPool *pmp,
	const WCHAR *wszFuncName
	)
{

	CHAR *szFuncName = CDXLUtils::SzFromWsz(pmp, wszFuncName);
	FuncCandidateList fcl = gpdb::FclFuncCandidates
								(
								ListMake1(gpdb::PvalMakeString(szFuncName)),
								-1 /* nargs */
								);

	if (NULL == fcl)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, wszFuncName);
	}

	delete [] szFuncName;
	return New(pmp) CMDIdGPDB(fcl->oid);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjRelStats
//
//	@doc:
//		Retrieve relation statistics from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjRelStats
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	CMDIdRelStats *pmdidRelStats = CMDIdRelStats::PmdidConvert(pmdid);
	IMDId *pmdidRel = pmdidRelStats->PmdidRel();
	OID oidRelation = CMDIdGPDB::PmdidConvert(pmdidRel)->OidObjectId();

	Relation rel = gpdb::RelGetRelation(oidRelation);
	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}
	// get rel name
	CHAR *szRelName = NameStr(rel->rd_rel->relname);
	CWStringDynamic *pstrRelName = CDXLUtils::PstrFromSz(pmp, szRelName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrRelName);
	// CMDName ctor created a copy of the string
	delete pstrRelName;

	CDouble dRows(rel->rd_rel->reltuples);
	pmdidRelStats->AddRef();
	CDXLRelStats *pdxlrelstats = New(pmp) CDXLRelStats
												(
												pmp,
												pmdidRelStats,
												pmdname,
												dRows
												);
	gpdb::CloseRelation(rel);

	return pdxlrelstats;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjColStats
//
//	@doc:
//		Retrieve column statistics from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjColStats
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	CMDIdColStats *pmdidColStats = CMDIdColStats::PmdidConvert(pmdid);
	IMDId *pmdidRel = pmdidColStats->PmdidRel();
	ULONG ulPos = pmdidColStats->UlPos();
	OID oidRelation = CMDIdGPDB::PmdidConvert(pmdidRel)->OidObjectId();

	Relation rel = gpdb::RelGetRelation(oidRelation);
	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	const IMDRelation *pmdrel = pmda->Pmdrel(pmdidRel);
	const IMDColumn *pmdcol = pmdrel->Pmdcol(ulPos);
	AttrNumber attrnum = (AttrNumber) pmdcol->IAttno();

	// number of rows from pg_class
	CDouble dRows(rel->rd_rel->reltuples);

	// extract column name and type
	CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
	OID oidAttType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType())->OidObjectId();
	gpdb::CloseRelation(rel);

	// extract out histogram and mcv information from pg_statistic
	DrgPdxlbucket *pdrgpdxlbucket = New(pmp) DrgPdxlbucket(pmp);
	HeapTuple heaptupleStats = gpdb::HtAttrStats(oidRelation, attrnum);

	// if there is no colstats
	if (!HeapTupleIsValid(heaptupleStats))
	{
		pdrgpdxlbucket->Release();
		pmdidColStats->AddRef();
		return CDXLColStats::PdxlcolstatsDummy
								(
								pmp,
								pmdidColStats,
								pmdnameCol
								);
	}


	Datum	   *pdrgdatumMCVValues = NULL;
	int			iNumMCVValues = 0;
	float4	   *pdrgfMCVFrequencies = NULL;
	int			iNumMCVFrequencies = 0;
	Datum		*pdrgdatumHistValues = NULL;
	int			iNumHistValues = 0;

	(void)	gpdb::FGetAttrStatsSlot
			(
					heaptupleStats,
					oidAttType,
					-1,
					STATISTIC_KIND_MCV,
					InvalidOid,
					&pdrgdatumMCVValues, &iNumMCVValues,
					&pdrgfMCVFrequencies, &iNumMCVFrequencies
			);

	GPOS_ASSERT(iNumMCVValues == iNumMCVFrequencies);

	// fix mcv frequencies (sometimes they can add up to more than 1.0)
	NormalizeFrequencies(pdrgfMCVFrequencies, (ULONG) iNumMCVValues);

	(void) gpdb::FGetAttrStatsSlot
			(
					heaptupleStats,
					oidAttType,
					-1,
					STATISTIC_KIND_HISTOGRAM,
					InvalidOid,
					&pdrgdatumHistValues, &iNumHistValues,
					NULL, NULL);

	Form_pg_statistic fpsStats = (Form_pg_statistic) GETSTRUCT(heaptupleStats);

	CDouble dWidth = CDouble(fpsStats->stawidth);

	// calculate total number of distinct values
	CDouble dDistinct(1.0);
	if (fpsStats->stadistinct < 0)
	{
		GPOS_ASSERT(fpsStats->stadistinct > -1.01);
		dDistinct = dRows * CDouble(-fpsStats->stadistinct);
	}
	else
	{
		dDistinct = CDouble(fpsStats->stadistinct);
	}
	dDistinct = dDistinct.FpCeil();

	// number of nulls
	CDouble dNullFrequency(fpsStats->stanullfrac);
	if (dNullFrequency < CStatistics::DEpsilon)
	{
		dNullFrequency = 0.0;
	}

	// total MCV frequency
	CDouble dMCFSum = 0.0;
	for (ULONG ul = 0; ul < iNumMCVValues; ul++)
	{
		dMCFSum = dMCFSum + CDouble(pdrgfMCVFrequencies[ul]);
	}

	// ndistinct and frequency of remaining tuples
	// that are not covered by MCV or histogram
	CDouble dDistinctRemain = 0.0;
	CDouble dFreqRemain = 0.0;

	// there will be remaining tuples when both conditions below are met:
	// 1. no GPDB histogram is available
	// 2. the MCVs do not cover all the tuples, i.e. sum(MCFs) < 1
	if (1 >= iNumHistValues && 1 - CStatistics::DEpsilon > dMCFSum)
	{
		dDistinctRemain = dDistinct - iNumMCVValues;
		dFreqRemain = 1 - dMCFSum;
	}

	// transform all the bits and pieces from pg_stats
	// to a single bucket structure
	DrgPdxlbucket *pdrgpdxlbucketTransformed =
			PdrgpdxlbucketTransformStats
					(
					pmp,
					oidAttType,
					dDistinct,
					pdrgdatumMCVValues,
					pdrgfMCVFrequencies,
					ULONG(iNumMCVValues),
					pdrgdatumHistValues,
					ULONG(iNumHistValues)
					);

	CUtils::AddRefAppend(pdrgpdxlbucket, pdrgpdxlbucketTransformed);

	// free up allocated datum and float4 arrays
	gpdb::FreeAttrStatsSlot(oidAttType, pdrgdatumMCVValues, iNumMCVValues, pdrgfMCVFrequencies, iNumMCVFrequencies);
	gpdb::FreeAttrStatsSlot(oidAttType, pdrgdatumHistValues, iNumHistValues, NULL, 0);

	gpdb::FreeHeapTuple(heaptupleStats);

	pdrgpdxlbucketTransformed->Release();

	// create col stats object
	pmdidColStats->AddRef();
	CDXLColStats *pdxlcolstats = New(pmp) CDXLColStats
											(
											pmp,
											pmdidColStats,
											pmdnameCol,
											dWidth,
											dNullFrequency,
											dDistinctRemain,
											dFreqRemain,
											pdrgpdxlbucket
											);

	return pdxlcolstats;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjCast
//
//	@doc:
//		Retrieve a cast function from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjCast
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	CMDIdCast *pmdidCast = CMDIdCast::PmdidConvert(pmdid);
	IMDId *pmdidSrc = pmdidCast->PmdidSrc();
	IMDId *pmdidDest = pmdidCast->PmdidDest();

	OID oidSrc = CMDIdGPDB::PmdidConvert(pmdidSrc)->OidObjectId();
	OID oidDest = CMDIdGPDB::PmdidConvert(pmdidDest)->OidObjectId();

	OID oidCastFunc = 0;
	BOOL fBinaryCoercible = false;
	
	BOOL fCastExists = gpdb::FCastFunc(oidSrc, oidDest, &fBinaryCoercible, &oidCastFunc);
	
	if (!fCastExists)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	} 
	
	CHAR *szFuncName = NULL;
	if (InvalidOid != oidCastFunc)
	{
		szFuncName = gpdb::SzFuncName(oidCastFunc);
	}
	else
	{
		// no explicit cast function: use the destination type name as the cast name
		szFuncName = gpdb::SzTypeName(oidDest);
	}
	
	if (NULL == szFuncName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	pmdid->AddRef();
	pmdidSrc->AddRef();
	pmdidDest->AddRef();

	CMDName *pmdname = CDXLUtils::PmdnameFromSz(pmp, szFuncName);
	
	return New(pmp) CMDCastGPDB(pmp, pmdid, pmdname, pmdidSrc, pmdidDest, fBinaryCoercible, New(pmp) CMDIdGPDB(oidCastFunc));
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdobjScCmp
//
//	@doc:
//		Retrieve a scalar comparison from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PmdobjScCmp
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	CMDIdScCmp *pmdidScCmp = CMDIdScCmp::PmdidConvert(pmdid);
	IMDId *pmdidLeft = pmdidScCmp->PmdidLeft();
	IMDId *pmdidRight = pmdidScCmp->PmdidRight();
	
	IMDType::ECmpType ecmpt = pmdidScCmp->Ecmpt();

	OID oidLeft = CMDIdGPDB::PmdidConvert(pmdidLeft)->OidObjectId();
	OID oidRight = CMDIdGPDB::PmdidConvert(pmdidRight)->OidObjectId();
	CmpType cmpt = (CmpType) UlCmpt(ecmpt);
	
	OID oidScCmp = gpdb::OidScCmp(oidLeft, oidRight, cmpt);
	
	if (InvalidOid == oidScCmp)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	} 

	CHAR *szName = gpdb::SzOpName(oidScCmp);

	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	pmdid->AddRef();
	pmdidLeft->AddRef();
	pmdidRight->AddRef();

	CMDName *pmdname = CDXLUtils::PmdnameFromSz(pmp, szName);

	return New(pmp) CMDScCmpGPDB(pmp, pmdid, pmdname, pmdidLeft, pmdidRight, ecmpt, New(pmp) CMDIdGPDB(oidScCmp));
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpdxlbucketTransformStats
//
//	@doc:
//		transform stats from pg_stats form to optimizer's preferred form
//
//---------------------------------------------------------------------------
DrgPdxlbucket *
CTranslatorRelcacheToDXL::PdrgpdxlbucketTransformStats
	(
	IMemoryPool *pmp,
	OID oidAttType,
	CDouble dDistinct,
	const Datum *pdrgdatumMCVValues,
	const float4 *pdrgfMCVFrequencies,
	ULONG ulNumMCVValues,
	const Datum *pdrgdatumHistValues,
	ULONG ulNumHistValues
	)
{
	CMDIdGPDB *pmdidAttType = New(pmp) CMDIdGPDB(oidAttType);
	IMDType *pmdtype = Pmdtype(pmp, pmdidAttType);

	// histogram from mcv information
	CHistogram *phistGPDBMCV = PhistTransformGPDBMCV
							(
							pmp,
							pmdtype,
							pdrgdatumMCVValues,
							pdrgfMCVFrequencies,
							ulNumMCVValues
							);

	GPOS_ASSERT(phistGPDBMCV->FValid());

	CHistogram *phistGPDBHist = NULL;
	CDouble dHistFreq = CDouble(1.0) - phistGPDBMCV->DFrequency();

	// TODO: shene - Jul 23, 2013; The semantics is not very clear here - if there are
	// no MCVs and no histogram, dHistFreq will be 1.0, which is counter-intuitive

	// if histogram has any significant information, then extract it
	if (dHistFreq > CStatistics::DEpsilon)
	{
		// histogram from gpdb histogram
		phistGPDBHist = PhistTransformGPDBHist
						(
						pmp,
						pmdtype,
						pdrgdatumHistValues,
						ulNumHistValues,
						dDistinct,
						dHistFreq
						);
	}

	DrgPdxlbucket *pdrgpdxlbucket = NULL;

	if (1 - CStatistics::DEpsilon < dHistFreq && 0 < ulNumHistValues)
	{
		// if histogram exists and dominates, use histogram only
		pdrgpdxlbucket = Pdrgpdxlbucket(pmp, pmdtype, phistGPDBHist);
	}

	else if (1 >= ulNumHistValues || CStatistics::DEpsilon > dHistFreq)
	{
		// if MCV dominates, use MCV only
		pdrgpdxlbucket = Pdrgpdxlbucket(pmp, pmdtype, phistGPDBMCV);
	}

	else
	{
		// otherwise, merge MCV and histogram buckets
		CHistogram *phistMerged = CStatisticsUtils::PhistMergeMcvHist(pmp, phistGPDBMCV, phistGPDBHist);
		pdrgpdxlbucket = Pdrgpdxlbucket(pmp, pmdtype, phistMerged);
		delete phistMerged;
	}

	// cleanup
	pmdidAttType->Release();
	pmdtype->Release();
	delete phistGPDBMCV;

	if (NULL != phistGPDBHist)
	{
		delete phistGPDBHist;
	}

	return pdrgpdxlbucket;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PhistTransformGPDBMCV
//
//	@doc:
//		Transform gpdb's mcv info to optimizer histogram
//
//---------------------------------------------------------------------------
CHistogram *
CTranslatorRelcacheToDXL::PhistTransformGPDBMCV
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const Datum *pdrgdatumMCVValues,
	const float4 *pdrgfMCVFrequencies,
	ULONG ulNumMCVValues
	)
{
	DrgPdatum *pdrgpdatum = New(pmp) DrgPdatum(pmp);
	DrgPdouble *pdrgpdFreq = New(pmp) DrgPdouble(pmp);

	for (ULONG ul = 0; ul < ulNumMCVValues; ul++)
	{
		Datum datumMCV = pdrgdatumMCVValues[ul];
		IDatum *pdatum = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false /* fNull */, datumMCV);
		pdrgpdatum->Append(pdatum);
		pdrgpdFreq->Append(New(pmp) CDouble(pdrgfMCVFrequencies[ul]));

		if (!pdatum->FHasStatsLessThan(pdatum))
		{
			// if less than operation is not supported on this datum, then no point
			// building a histogram. return an empty histogram
			pdrgpdatum->Release();
			pdrgpdFreq->Release();
			return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp));
		}
	}

	CHistogram *phist = CStatisticsUtils::PhistTransformMCV
												(
												pmp,
												pmdtype,
												pdrgpdatum,
												pdrgpdFreq,
												ulNumMCVValues
												);

	pdrgpdatum->Release();
	pdrgpdFreq->Release();
	return phist;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PhistTransformGPDBHist
//
//	@doc:
//		Transform GPDB's hist info to optimizer's histogram
//
//---------------------------------------------------------------------------
CHistogram *
CTranslatorRelcacheToDXL::PhistTransformGPDBHist
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const Datum *pdrgdatumHistValues,
	ULONG ulNumHistValues,
	CDouble dDistinctHist,
	CDouble dFreqHist
	)
{
	// handle corner case of no histogram
	if (ulNumHistValues <= 1)
	{
		return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp), false /* fWellDefined */);
	}

	GPOS_ASSERT(ulNumHistValues > 1);
	ULONG ulNumBuckets = ulNumHistValues - 1;
	CDouble dDistinctPerBucket = dDistinctHist / CDouble(ulNumBuckets);
	CDouble dFreqPerBucket = dFreqHist / CDouble(ulNumBuckets);

	const ULONG ulBuckets = ulNumHistValues - 1;
	// create buckets
	DrgPbucket *pdrgppbucket = New(pmp) DrgPbucket(pmp);
	for (ULONG ul = 0; ul < ulBuckets; ul++)
	{
		Datum datumMin = pdrgdatumHistValues[ul];
		IDatum *pdatumMin = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false /* fNull */, datumMin);

		Datum datumMax = pdrgdatumHistValues[ul + 1];
		IDatum *pdatumMax = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false /* fNull */, datumMax);

		BOOL fLowerClosed = true; // GPDB histograms assumes lower bound to be closed
		BOOL fUpperClosed = false; // GPDB histograms assumes upper bound to be open
		if (ul == ulBuckets - 1)
		{
			// last bucket upper bound is also closed
			fUpperClosed = true;
		}

		CBucket *pbucket = New(pmp) CBucket
									(
									New(pmp) CPoint(pdatumMin),
									New(pmp) CPoint(pdatumMax),
									fLowerClosed,
									fUpperClosed,
									dFreqPerBucket,
									dDistinctPerBucket
									);
		pdrgppbucket->Append(pbucket);

		if (!pdatumMin->FHasStatsLessThan(pdatumMin))
		{
			// if less than operation is not supported on this datum, then no point
			// building a histogram. return an empty histogram
			pdrgppbucket->Release();
			return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp));
		}
	}

	CHistogram *phist = New(pmp) CHistogram(pdrgppbucket);
	return phist;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pdrgpdxlbucket
//
//	@doc:
//		Histogram to array of dxl buckets
//
//---------------------------------------------------------------------------
DrgPdxlbucket *
CTranslatorRelcacheToDXL::Pdrgpdxlbucket
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const CHistogram *phist
	)
{
	DrgPdxlbucket *pdrgpdxlbucket = New(pmp) DrgPdxlbucket(pmp);
	const DrgPbucket *pdrgpbucket = phist->Pdrgpbucket();
	ULONG ulNumBuckets = pdrgpbucket->UlLength();
	for (ULONG ul = 0; ul < ulNumBuckets; ul++)
	{
		CBucket *pbucket = (*pdrgpbucket)[ul];
		IDatum *pdatumLB = pbucket->PpLower()->Pdatum();
		CDXLDatum *pdxldatumLB = pmdtype->Pdxldatum(pmp, pdatumLB);
		IDatum *pdatumUB = pbucket->PpUpper()->Pdatum();
		CDXLDatum *pdxldatumUB = pmdtype->Pdxldatum(pmp, pdatumUB);
		CDXLBucket *pdxlbucket = New (pmp) CDXLBucket
											(
											pdxldatumLB,
											pdxldatumUB,
											pbucket->FLowerClosed(),
											pbucket->FUpperClosed(),
											pbucket->DFrequency(),
											pbucket->DDistinct()
											);
		pdrgpdxlbucket->Append(pdxlbucket);
	}
	return pdrgpdxlbucket;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Erelstorage
//
//	@doc:
//		Get relation storage type
//
//---------------------------------------------------------------------------
IMDRelation::Erelstoragetype
CTranslatorRelcacheToDXL::Erelstorage
	(
	CHAR cStorageType
	)
{
	IMDRelation::Erelstoragetype erelstorage = IMDRelation::ErelstorageSentinel;

	switch (cStorageType)
	{
		case RELSTORAGE_HEAP:
			erelstorage = IMDRelation::ErelstorageHeap;
			break;
		case RELSTORAGE_AOCOLS:
			erelstorage = IMDRelation::ErelstorageAppendOnlyCols;
			break;
		case RELSTORAGE_AOROWS:
			erelstorage = IMDRelation::ErelstorageAppendOnlyRows;
			break;
		case RELSTORAGE_PARQUET:
			erelstorage = IMDRelation::ErelstorageAppendOnlyParquet;
			break;
		case RELSTORAGE_VIRTUAL:
			erelstorage = IMDRelation::ErelstorageVirtual;
			break;
		case RELSTORAGE_EXTERNAL:
			erelstorage = IMDRelation::ErelstorageExternal;
			break;
		default:
			GPOS_ASSERT(!"Unsupported relation type");
	}

	return erelstorage;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpulPartKeys
//
//	@doc:
//		Get partition keys for relation or NULL if relation not partitioned
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorRelcacheToDXL::PdrgpulPartKeys
	(
	IMemoryPool *pmp,
	OID oid
	)
{
	if (!gpdb::FRelPartIsRoot(oid))
	{
		// not a partitioned table
		return NULL;
	}

	// TODO: antovl - Feb 23, 2012; support intermediate levels

	DrgPul *pdrgpulPartKeys = New(pmp) DrgPul(pmp);

	MemoryContext mcxt = CurrentMemoryContext;
	PartitionNode *pn = gpdb::PpnParts(oid, 0 /*level*/, 0 /*parent*/, false /*inctemplate*/, mcxt);
	GPOS_ASSERT(NULL != pn);
	
	if (gpdb::FHashPartitioned(pn->part->parkind))
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported, GPOS_WSZ_LIT("Hash partitioning"));
	}
	
	List *plPartKeys = gpdb::PlPartitionAttrs(oid);

	ListCell *plc = NULL;
	ForEach (plc, plPartKeys)
	{
		INT iAttno = lfirst_int(plc);
		GPOS_ASSERT(0 < iAttno);
		pdrgpulPartKeys->Append(New(pmp) ULONG(iAttno - 1));
	}

	return pdrgpulPartKeys;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PulAttnoMapping
//
//	@doc:
//		Construct a mapping for GPDB attnos to positions in the columns array
//
//---------------------------------------------------------------------------
ULONG *
CTranslatorRelcacheToDXL::PulAttnoMapping
	(
	IMemoryPool *pmp,
	DrgPmdcol *pdrgpmdcol,
	ULONG ulMaxCols
	)
{
	GPOS_ASSERT(NULL != pdrgpmdcol);
	GPOS_ASSERT(0 < pdrgpmdcol->UlLength());
	GPOS_ASSERT(ulMaxCols > pdrgpmdcol->UlLength());

	// build a mapping for attnos->positions
	const ULONG ulCols = pdrgpmdcol->UlLength();
	ULONG *pul = New(pmp) ULONG[ulMaxCols];

	// initialize all positions to ULONG_MAX
	for (ULONG ul = 0;  ul < ulMaxCols; ul++)
	{
		pul[ul] = ULONG_MAX;
	}
	
	for (ULONG ul = 0;  ul < ulCols; ul++)
	{
		const IMDColumn *pmdcol = (*pdrgpmdcol)[ul];
		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		pul[ulIndex] = ul;
	}

	return pul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpdrgpulKeys
//
//	@doc:
//		Get key sets for relation
//
//---------------------------------------------------------------------------
DrgPdrgPul *
CTranslatorRelcacheToDXL::PdrgpdrgpulKeys
	(
	IMemoryPool *pmp,
	OID oid,
	BOOL fAddDefaultKeys,
	ULONG *pulMapping
	)
{
	DrgPdrgPul *pdrgpdrgpul = New(pmp) DrgPdrgPul(pmp);

	List *plKeys = gpdb::PlRelationKeys(oid);

	ListCell *plcKey = NULL;
	ForEach (plcKey, plKeys)
	{
		List *plKey = (List *) lfirst(plcKey);

		DrgPul *pdrgpulKey = New(pmp) DrgPul(pmp);

		ListCell *plcKeyElem = NULL;
		ForEach (plcKeyElem, plKey)
		{
			INT iKey = lfirst_int(plcKeyElem);
			ULONG ulPos = UlPosition(iKey, pulMapping);
			pdrgpulKey->Append(New(pmp) ULONG(ulPos));
		}
		GPOS_ASSERT(0 < pdrgpulKey->UlLength());

		pdrgpdrgpul->Append(pdrgpulKey);
	}
	
	// add {segid, ctid} as a key
	
	if (fAddDefaultKeys)
	{
		DrgPul *pdrgpulKey = New(pmp) DrgPul(pmp);
		ULONG ulPosSegid= UlPosition(GpSegmentIdAttributeNumber, pulMapping);
		ULONG ulPosCtid = UlPosition(SelfItemPointerAttributeNumber, pulMapping);
		pdrgpulKey->Append(New(pmp) ULONG(ulPosSegid));
		pdrgpulKey->Append(New(pmp) ULONG(ulPosCtid));
		
		pdrgpdrgpul->Append(pdrgpulKey);
	}
	
	return pdrgpdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::NormalizeFrequencies
//
//	@doc:
//		Sometimes a set of frequencies can add up to more than 1.0.
//		Fix these cases
//
//---------------------------------------------------------------------------
void
CTranslatorRelcacheToDXL::NormalizeFrequencies
	(
	float4 *pdrgf,
	ULONG ulLength
	)
{
	if (ulLength == 0)
	{
		return;
	}

	CDouble dTotal = CDouble(pdrgf[0]);
	for (ULONG ul=1; ul<ulLength; ul++)
	{
		dTotal = dTotal + CDouble(pdrgf[ul]);
	}

	if (dTotal > CDouble(1.0))
	{
		float4 fDenom = (float4) (dTotal + CStatistics::DEpsilon).DVal();

		// divide all values by the total
		for (ULONG ul=0; ul<ulLength; ul++)
		{
			pdrgf[ul] = pdrgf[ul] / fDenom;
		}
	}

#ifdef GPOS_DEBUG
	// recheck
	CDouble dTotalRecheck = CDouble(pdrgf[0]);
	for (ULONG ul=1; ul<ulLength; ul++)
	{
		dTotalRecheck = dTotalRecheck + CDouble(pdrgf[ul]);
	}
	GPOS_ASSERT(dTotalRecheck <= CDouble(1.0));
#endif
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::FIndexSupported
//
//	@doc:
//		Check if index type is supported
//
//---------------------------------------------------------------------------
BOOL
CTranslatorRelcacheToDXL::FIndexSupported
	(
	Relation relIndex
	)
{
	HeapTupleData *pht = relIndex->rd_indextuple;
	
	// index expressions and index constraints not supported
	return gpdb::FHeapAttIsNull(pht, Anum_pg_index_indexprs) &&
		gpdb::FHeapAttIsNull(pht, Anum_pg_index_indpred) && 
		BTREE_AM_OID == relIndex->rd_rel->relam;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdpartcnstrIndex
//
//	@doc:
//		Retrieve part constraint for index
//
//---------------------------------------------------------------------------
CMDPartConstraintGPDB *
CTranslatorRelcacheToDXL::PmdpartcnstrIndex
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	const IMDRelation *pmdrel,
	Node *pnodePartCnstr, 
	BOOL fDefaultPartition,
	BOOL fUnbounded
	)
{
	DrgPdxlcd *pdrgpdxlcd = New(pmp) DrgPdxlcd(pmp);
	const ULONG ulColumns = pmdrel->UlColumns();
	
	for (ULONG ul = 0; ul < ulColumns; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		CMDIdGPDB *pmdidColType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType());
		pmdidColType->AddRef();

		// create a column descriptor for the column
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdnameCol,
										ul + 1, // ulColId
										pmdcol->IAttno(),
										pmdidColType,
										false // fColDropped
										);
		pdrgpdxlcd->Append(pdxlcd);
	}
	
	CMDPartConstraintGPDB *pmdpartcnstr = PmdpartcnstrFromNode(pmp, pmda, pdrgpdxlcd, pnodePartCnstr, fDefaultPartition, fUnbounded);
	
	pdrgpdxlcd->Release();

	return pmdpartcnstr;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdpartcnstrRelation
//
//	@doc:
//		Retrieve part constraint for relation
//
//---------------------------------------------------------------------------
CMDPartConstraintGPDB *
CTranslatorRelcacheToDXL::PmdpartcnstrRelation
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	OID oidRel,
	DrgPmdcol *pdrgpmdcol
	)
{
	// get the part constraints
	BOOL fDefaultPartition = false;
	Node *pnode = gpdb::PnodePartConstraintRel(oidRel, &fDefaultPartition);

	DrgPdxlcd *pdrgpdxlcd = New(pmp) DrgPdxlcd(pmp);
	const ULONG ulColumns = pdrgpmdcol->UlLength();
	for (ULONG ul = 0; ul < ulColumns; ul++)
	{
		const IMDColumn *pmdcol = (*pdrgpmdcol)[ul];
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		CMDIdGPDB *pmdidColType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType());
		pmdidColType->AddRef();

		// create a column descriptor for the column
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdnameCol,
										ul + 1, // ulColId
										pmdcol->IAttno(),
										pmdidColType,
										false // fColDropped
										);
		pdrgpdxlcd->Append(pdxlcd);
	}
	
	CMDPartConstraintGPDB *pmdpartcnstr = PmdpartcnstrFromNode(pmp, pmda, pdrgpdxlcd, pnode, fDefaultPartition, fDefaultPartition /*fUnbounded*/);
	
	pdrgpdxlcd->Release();

	return pmdpartcnstr;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdpartcnstrFromNode
//
//	@doc:
//		Retrieve part constraint from GPDB node
//
//---------------------------------------------------------------------------
CMDPartConstraintGPDB *
CTranslatorRelcacheToDXL::PmdpartcnstrFromNode
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	DrgPdxlcd *pdrgpdxlcd,
	Node *pnodeCnstr,
	BOOL fDefaultPartition,
	BOOL fUnbounded
	)
{
	if (NULL == pnodeCnstr)
	{
		return NULL;
	}

	CTranslatorScalarToDXL sctranslator
							(
							pmp,
							pmda,
							NULL, // pulidgtorCol
							NULL, // pulidgtorCTE
							0, // ulQueryLevel
							true, // m_fQuery
							NULL, // m_pplstmt
							NULL, // m_pmappv
							NULL, // phmulCTEEntries
							NULL // pdrgpdxlnCTE
							);

	// generate a mock mapping between var to column information
	CMappingVarColId *pmapvarcolid = New(pmp) CMappingVarColId(pmp);

	pmapvarcolid->LoadColumns(0 /*ulQueryLevel */, 1 /* rteIndex */, pdrgpdxlcd);

	// translate the check constraint expression
	CDXLNode *pdxlnScalar = sctranslator.PdxlnScOpFromExpr((Expr *) pnodeCnstr, pmapvarcolid);

	// cleanup
	delete pmapvarcolid;

	return New(pmp) CMDPartConstraintGPDB(pdxlnScalar, fDefaultPartition, fUnbounded);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::FHasSystemColumns
//
//	@doc:
//		Does given relation type have system columns.
//		Currently only regular relations, sequences, toast values relations and
//		AO segment relations have system columns
//
//---------------------------------------------------------------------------
BOOL
CTranslatorRelcacheToDXL::FHasSystemColumns
	(
	char cRelKind
	)
{
	return RELKIND_RELATION == cRelKind || 
			RELKIND_SEQUENCE == cRelKind || 
			RELKIND_AOSEGMENTS == cRelKind ||
			RELKIND_TOASTVALUE == cRelKind;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Ecmpt
//
//	@doc:
//		Translate GPDB comparison types into optimizer comparison types
//
//---------------------------------------------------------------------------
IMDType::ECmpType
CTranslatorRelcacheToDXL::Ecmpt
	(
	ULONG ulCmpt
	)
{
	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(rgulCmpTypeMappings); ul++)
	{
		const ULONG *pul = rgulCmpTypeMappings[ul];
		if (pul[1] == ulCmpt)
		{
			return (IMDType::ECmpType) pul[0];
		}
	}
	
	return IMDType::EcmptOther;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::UlCmpt
//
//	@doc:
//		Translate optimizer comparison types into GPDB comparison types
//
//---------------------------------------------------------------------------
ULONG 
CTranslatorRelcacheToDXL::UlCmpt
	(
	IMDType::ECmpType ecmpt
	)
{
	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(rgulCmpTypeMappings); ul++)
	{
		const ULONG *pul = rgulCmpTypeMappings[ul];
		if (pul[0] == ecmpt)
		{
			return (ULONG) pul[1];
		}
	}
	
	return CmptOther;
}

// EOF

