/*--------------------------------------------------------------------------
 *
 * cdbpartindex.c
 *	Provides utility routines to support indexes over partitioned tables
 *	within Greenplum Database.
 *
 * Copyright (c) 2005-2010, Greenplum inc
 *
 *--------------------------------------------------------------------------
 */
#include "postgres.h"
#include "funcapi.h"
#include "fmgr.h"
#include "access/catquery.h"
#include "access/hash.h"
#include "catalog/index.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_partition_encoding.h"
#include "catalog/pg_partition_rule.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbvars.h"
#include "commands/tablecmds.h"
#include "optimizer/planmain.h" 
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/* initial estimate for number of logical indexes */
#define INITIAL_NUM_LOGICAL_INDEXES_ESTIMATE 100

static int logicalIndexId = 1;
static int numLogicalIndexes = 0;
static int numIndexesOnDefaultParts = 0;

/* 
 * hash table to identify distinct "logical indexes". Each index with the same
 * index key, index pred, index expr is considered equivalent.
 */
static HTAB *PartitionIndexHash;

/* hash table to hold information about distinct logical indexes. */
static HTAB *LogicalIndexInfoHash;

/* 
 * Hash entry for PartionIndexHash
 * hashkey is (indexkey + indexpred + indexexprs)
 */
typedef struct 
{ 
	char 		*key;
	int 		logicalIndexId;
} PartitionIndexHashEntry;

/*
 * Hashentry for LogicalIndexInfoHash
 * hashkey is (logicalIndexId)
 */
typedef struct
{
	char 		*key;
	int 		logicalIndexId;
	Oid		logicalIndexOid;
	struct 		IndexInfo *ii;
	List		*partList;
	List		*defaultPartList;
} LogicalIndexInfoHashEntry;

/*
 * Each node in the PartitionIndexNode tree describes a partition
 * and a list of it's logical indexes
 */
typedef struct PartitionIndexNode
{
	Oid		paroid;		/* OID of row in pg_partition */
	Oid		parparentrule;	/* OID of row in pg_partition_rule */
	Oid 		parchildrelid;	/* table oid of the part */
	Oid		parrelid;	/* needed to map attnums */
	bool		isDefault;	/* is this a default part */
	Bitmapset	*index; 	/* logical indexes for this part */
	List		*children;	/* child parts */
	List		*defaultLevels; /* the level at which the part key is default */
} PartitionIndexNode;

static void recordIndexesOnLeafPart(PartitionIndexNode **pNodePtr,
					Oid partOid, Oid rootOid,
					Relation indexRel);
static void recordIndexes(PartitionIndexNode **partIndexTree,
					Relation indexRel);
static void getPartitionIndexNode(Oid rootOid, int2 level,
					Oid parent, PartitionIndexNode **n,
					bool isDefault, List *defaultLevels);
static void indexParts(PartitionIndexNode **np, bool isDefault);
static void dumpPartsIndexInfo(PartitionIndexNode *n, int level);
static LogicalIndexes * createPartsIndexResult(Oid root);
static bool collapseIndexes(PartitionIndexNode **partitionIndexNode,
					LogicalIndexInfoHashEntry **entry);
static void createIndexHashTables(void);
static IndexInfo *populateIndexInfo(cqContext *pcqCtx, HeapTuple tuple,
					Form_pg_index indForm);

/*
 * TODO: similar routines in cdbpartition.c. Move up to cdbpartition.h ?
 */

/* Hash entire string */
static uint32
key_string_hash(const void *key, Size keysize)
{
	Size		s_len = strlen((const char *) key);
	
	Assert(keysize == sizeof(char*));
	return DatumGetUInt32(hash_any((const unsigned char *) key, (int) s_len));
}

/* Compare entire string. */
static int
key_string_compare(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(char*));
	return strcmp(((PartitionIndexHashEntry*)key1)->key, key2);
}

/* Copy string by copying pointer. */
static void *
key_string_copy(void *dest, const void *src, Size keysize)
{
	Assert(keysize == sizeof(char*));
	
	*((char**)dest) = (char*)src; /* trust caller re allocation */
	return NULL; /* not used */
}

/*
 * BuildLogicalIndexInfo
 *   Returns an array of "logical indexes" on partitioned
 *   tables.
 *
 *   Memory for result is allocated from caller context.
 *
 *   If input relid does not represent a partitioned table, the function
 *   returns NULL.
 * 
 *   This function does the following
 *	1) Builds a PartitionIndexNode tree representing the partitioned
 *	   table hierarchy.
 * 	2) Builds 2 hash tables -
 *	 	- 1st hash table is used to find the logical indexes in a
 *		  partitioning tree hierarchy - a logical
 * 		  index corresponds to multiple physical indexes each
 *		  of which have the same index key + index pred + index exprs. Each
 *		  logical index is assigned a unique logicalIndexId.
 *		- The 2nd hash table table is used to hold information associated
 *		  with a logical index for easy lookup.
 *	3) Annotates leaves of hierarchy with logical index ids.
 *	   See recordIndexes function comments.
 *	4) Consolidates logical index information i.e. if a logical index exists on all
 * 	   child parts of a parent part, the parent part is annotated with the logical
 *	   index id and logical index id is cleared from child parts.
 *	5) For each logical index, OR the check constraints of the part on which
 * 	   the index exists (i.e. generate a partial index predicate for each
 *	   logical index)
 * 	6) Since default parts, don't have constraints associated with them,
 * 	   each logical index on a default part is represented as a separate
 *	   logical index in the output. Some additional information is
 *	   included so the specific default partition may be identified.
 * 
 *	Example of a partitioning hierarchy, and the output generated.
 *	I1..In are logical index ids.
 *
 *	After Step 3 (leaves annotated with logical index ids)
 *
 *				Root 
 *		  / 		|		\
 *		 /		|		 \
 *		/		|		  \
 *		A1 		A2		default  - pk1 is partkey at level 0
 *	     /      \	       /    \		/       \
 *	    /	     \        /      \	       /         \
 *	   /          \	     /        \	      /           \
 * B1(I1,I2)   def(I1,I2) B1(I1,I2)  def(I2) B1(I1,I2,I4) def(I1,I2,I3) - pk2 is partkey at level 1
 *
 *
 * 	After step 4 - (consolidation of logical indexes)
 *
 *			      Root (I2)
 *		 / 		| 	       \
 *		/		|		\ 
 * 	       /             	|		 \ 
 *   	    A1 (I1)		A2		default	(I1)
 *	 /      \            /      \            /     \
 *	/        \          /        \	 	/       \
 *     /          \	   /          \	       /         \
 *    B1         def   B1(I1)         def   B1(I4)    def(I3)
 *
 * The output returned from BuildLogicalIndexInfo will be
 * 
 * logicalIndexOid 	nColumns  indexKeys  indPred indExprs  indIsUnique  partCons   				defaultLevels
 * 	I2		..	  ..	     ..	     ..	       ..           -						-
 * 	I1		..	  ..	     ..	     ..	       ..           pk1=A1 OR (pk1=A2 AND pk2=B1)		- 
 * 	I1'		..	  ..	     ..	     ..	       ..           -						0
 * 	I3		..	  ..	     ..	     ..	       ..           -						0,1
 * 	I4		..	  ..	     ..	     ..	       ..           pk2=B1	 				0
 */
LogicalIndexes *
BuildLogicalIndexInfo(Oid relid)
{
	MemoryContext   callerContext = NULL;
	MemoryContext   partContext = NULL;
	LogicalIndexes *partsLogicalIndexes = NULL;
	Relation	indexRel;
	HASH_SEQ_STATUS hash_seq;
	LogicalIndexInfoHashEntry *entry;
	PartitionIndexNode *n = NULL;

	/*
	 * create a memory context to hold allocations, so we can get rid of
	 * at the end of this function.
	 */
	partContext = AllocSetContextCreate(CurrentMemoryContext,
					"Partitioned Logical Index Context",
					ALLOCSET_DEFAULT_MINSIZE,
					ALLOCSET_DEFAULT_INITSIZE,
					ALLOCSET_DEFAULT_MAXSIZE);

	/* switch to the new context */
	callerContext = MemoryContextSwitchTo(partContext);

	/* initialize globals */
	logicalIndexId = 1;
	numLogicalIndexes = 0;
	numIndexesOnDefaultParts = 0;

	/* construct the partition node tree */
	getPartitionIndexNode(relid, 0, InvalidOid, &n, false, NIL);

	if (!n)
		return NULL;

	/* create the hash tables to hold the logical index info */
	createIndexHashTables();
	
	/* open pg_index and pass it to recordIndexes */
	indexRel = heap_open(IndexRelationId, AccessShareLock);

	/* now walk the tree and annotate with logical index id at each leaf */
	recordIndexes(&n, indexRel);

	heap_close(indexRel, AccessShareLock);
	hash_freeze(LogicalIndexInfoHash);

	/* get rid of first hash table here as we have identified all logical indexes */
	hash_destroy(PartitionIndexHash);
	
	/* walk the tree and pull-up indexes for each logical index */
	hash_seq_init(&hash_seq, LogicalIndexInfoHash);
	while ((entry = hash_seq_search(&hash_seq)))
	{
		collapseIndexes(&n, &entry);
		numLogicalIndexes++;
	}

	dumpPartsIndexInfo(n, 0);

	/* associate index with parts */
	indexParts(&n, false);

	/* switch to caller context and handle results */
	MemoryContextSwitchTo(callerContext);

	/* generate output rows */
	if ((numLogicalIndexes+numIndexesOnDefaultParts) > 0)
		partsLogicalIndexes = createPartsIndexResult(relid);

	hash_destroy(LogicalIndexInfoHash);

	/* all information has been copied into caller context */
	MemoryContextDelete(partContext);
	
	return partsLogicalIndexes;
}

/* 
 * getPartitionIndexNode
 *   Construct a PartitionIndexNode tree for the given part.
 *   Recurs to construct branches. 
 */
static void
getPartitionIndexNode(Oid rootOid,
			int2 level,
			Oid parent,
			PartitionIndexNode **n,
			bool isDefault,
			List *defaultLevels)
{
	HeapTuple	tuple;
	cqContext	*pcqCtx;
	cqContext	cqc;
	Relation	partRel;
	Relation	partRuleRel;
	Form_pg_partition partDesc;
	Oid		parOid;
	bool 		inctemplate = false;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;
	
	/* select oid as partid, *
	 * from pg_partition
	 * where 
   	 * 	parrelid = :relid and
	 * 	parlevel = :level and
	 *	paristemplate = false;
	 */
	partRel = heap_open(PartitionRelationId, AccessShareLock);

	tuple = caql_getfirst(
			caql_addrel(cqclr(&cqc), partRel),
			cql("SELECT * FROM pg_partition "
				" WHERE parrelid = :1 "
				" AND parlevel = :2 "
				" AND paristemplate = :3",
				ObjectIdGetDatum(rootOid),
				Int16GetDatum(level),
				Int16GetDatum(inctemplate)));

	if (HeapTupleIsValid(tuple))
	{
		parOid = HeapTupleGetOid(tuple);
		partDesc = (Form_pg_partition) GETSTRUCT(tuple);

		if (level == 0)
		{
			Assert(parent == InvalidOid);
			Assert(*n == NULL);

			/* handle root part specially */
			*n = palloc0(sizeof(PartitionIndexNode));
			(*n)->paroid = parOid;
			(*n)->parrelid = (*n)->parchildrelid = partDesc->parrelid;
			(*n)->isDefault = false;
		}
		heap_close(partRel, AccessShareLock);
	}
	else
	{
		heap_close(partRel, AccessShareLock);
		return;
	}


	/* recurse to fill the children */
	partRuleRel = heap_open(PartitionRuleRelationId, AccessShareLock);

	pcqCtx = caql_beginscan(
                        caql_addrel(cqclr(&cqc), partRuleRel),
                        cql("SELECT * FROM pg_partition_rule "
                                " WHERE paroid = :1 "
                                " AND parparentrule = :2 ",
                                ObjectIdGetDatum(parOid),
                                ObjectIdGetDatum(parent)));

	while (HeapTupleIsValid(tuple = caql_getnext(pcqCtx)))
        {    
                PartitionIndexNode *child;
		Form_pg_partition_rule rule_desc = (Form_pg_partition_rule)GETSTRUCT(tuple);
	
		child = palloc(sizeof(PartitionIndexNode));
		memset(child, 0, sizeof(PartitionIndexNode));
		child->paroid = HeapTupleGetOid(tuple);
		child->parrelid = partDesc->parrelid;
		child->parchildrelid = rule_desc->parchildrelid;

		/* For every node, we keep track of every level at which this node has default partitioning */
		child->isDefault = isDefault;
		child->defaultLevels = defaultLevels;

		/* current part is a default part */
		if (rule_desc->parisdefault)
		{
			child->isDefault = true;
			child->defaultLevels = lappend_int(child->defaultLevels, partDesc->parlevel);
		}

		/* insert child into children */
		(*n)->children = lappend((*n)->children, child);

		getPartitionIndexNode(rootOid, level + 1, HeapTupleGetOid(tuple),
					&child, child->isDefault, child->defaultLevels);
	}

	caql_endscan(pcqCtx);
	heap_close(partRuleRel, AccessShareLock);
}

/* 
 * constructIndexHashKey
 *   The index hash key is a string representing the index key,
 *   index predicate, and index exprs
 *
 *   Note, all attnums are mapped to the root.
 */ 
static char *
constructIndexHashKey(Oid partOid,
			Oid rootOid,
			HeapTuple tup,
			AttrNumber *attMap,
			IndexInfo *ii)
{
	StringInfoData  buf;
	Expr 		*predExpr = NULL;
	Expr 		*exprsExpr = NULL;
	
	initStringInfo(&buf);

	/* map the attrnos in the indKey to root part */
	for (int i = 0; i < ii->ii_NumIndexAttrs; i++)
	{
		if (ii->ii_KeyAttrNumbers[i] != 0)
			ii->ii_KeyAttrNumbers[i] = attMap[(ii->ii_KeyAttrNumbers[i]) - 1];
		appendStringInfo(&buf, "%d", ii->ii_KeyAttrNumbers[i]);
	}	

	/* map the attrnos in indPred */
	predExpr = (Expr *)ii->ii_Predicate;
	
	change_varattnos_of_a_node((Node *)predExpr, attMap);

	appendStringInfo(&buf, "%s", nodeToString(predExpr));

	/* remember mapped predicate */
	ii->ii_Predicate = (List *)predExpr;

	exprsExpr = (Expr *)ii->ii_Expressions;

	change_varattnos_of_a_node((Node *)exprsExpr, attMap);

	appendStringInfo(&buf, "%s", nodeToString(exprsExpr));

	/* remember mapped exprsList */
	ii->ii_Expressions = (List *)exprsExpr;

	return buf.data;
} 

/* 
 * createIndexHashTables
 *  create the hash tables to hold logical indexes 
 */
static void
createIndexHashTables()
{
	MemoryContext	context = NULL;
	HASHCTL		hash_ctl;

	/* 
	 * this hash is to identify the logical indexes. 1 logical index corresponds to
	 * multiple physical indexes which have the same indkey + indpred + indexprs
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(char *);
	hash_ctl.entrysize = sizeof(PartitionIndexHashEntry);
	hash_ctl.hash = key_string_hash;
	hash_ctl.match = key_string_compare;
	hash_ctl.keycopy = key_string_copy;
	hash_ctl.hcxt = context;
	PartitionIndexHash = hash_create("Partition Index Hash",
				INITIAL_NUM_LOGICAL_INDEXES_ESTIMATE,
				&hash_ctl,
				HASH_ELEM | HASH_FUNCTION |
				HASH_COMPARE | HASH_KEYCOPY |
				HASH_CONTEXT);

	Assert(PartitionIndexHash != NULL);

	/*  this hash is used to hold the logical indexes for easy lookup */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(uint32);
	hash_ctl.entrysize = sizeof(LogicalIndexInfoHashEntry);
	hash_ctl.hash = tag_hash;
	hash_ctl.hcxt = context;
	LogicalIndexInfoHash = hash_create("Logical Index Info Hash",
				INITIAL_NUM_LOGICAL_INDEXES_ESTIMATE,
				&hash_ctl,
				HASH_ELEM | HASH_FUNCTION );
	Assert(LogicalIndexInfoHash != NULL);
}

/*
 * recordIndexes
 *   Invoke the recordIndexesOnPart routine on every leaf partition.
 */
static void
recordIndexes(PartitionIndexNode **partIndexTree, Relation indexRel)
{
	ListCell *lc; 
	PartitionIndexNode *partIndexNode = *partIndexTree;

	if (partIndexNode->children)
	{
		foreach (lc, partIndexNode->children)
		{
			PartitionIndexNode *child = (PartitionIndexNode *) lfirst(lc);
			recordIndexes(&child, indexRel);
		}
	}
	else 
		/* at a leaf */
		recordIndexesOnLeafPart(partIndexTree, 
					partIndexNode->parchildrelid, 
					partIndexNode->parrelid, indexRel);
}

/*
 * recordIndexesOnLeafPart 
 *   Given a part
 * 	- fetch each index on the part from pg_index
 *	- map attnums in index key, index expr, index pred to the root attnums
 *	- insert the logical index into the PartitionIndexHash using the
 *		index key, index exprs, index pred as hash  key.
 *	- fetch the associated logical index id from the hash and record it in the tree.
 *	- insert the logical index into the LogicalIndexInfo Hash using the logical index id
 * 	  as the hash key.
 */
static void
recordIndexesOnLeafPart(PartitionIndexNode **pNodePtr,
			Oid partOid,
			Oid rootOid,
			Relation indexRel)
{
	HeapTuple 			tuple; 
	cqContext 			*pcqCtx;
	cqContext 			cqc;
	char 				*partIndexHashKey;
	bool 				foundPartIndexHash;
	bool 				foundLogicalIndexHash;
	AttrNumber 			*attmap = NULL;
	struct IndexInfo 		*ii = NULL;
	PartitionIndexHashEntry 	*partIndexHashEntry;
	LogicalIndexInfoHashEntry	*logicalIndexInfoHashEntry;
	PartitionIndexNode		*pNode = *pNodePtr;
	
	pcqCtx = caql_beginscan(
			caql_addrel(cqclr(&cqc), indexRel),
			cql("SELECT * FROM pg_index "
				" WHERE indrelid = :1",
			ObjectIdGetDatum(partOid)));

	/* fetch each index on part */
	while (HeapTupleIsValid(tuple = caql_getnext(pcqCtx)))
	{
		Form_pg_index indForm = (Form_pg_index) GETSTRUCT(tuple);

		/* 
		 * when constructing hash key, we need to map attnums in part indexes
		 * to root attnums. Get the attMap needed for mapping.
		 */
		if (!attmap)
		{
			Relation rootRel = heap_open(rootOid, AccessShareLock);
			Relation partRel = heap_open(partOid, AccessShareLock);

			TupleDesc rootTupDesc = rootRel->rd_att;
			TupleDesc partTupDesc = partRel->rd_att;
		
			attmap = varattnos_map(partTupDesc, rootTupDesc);

			/* can we close here ? */
			heap_close(rootRel, AccessShareLock);
			heap_close(partRel, AccessShareLock);
		}

		/* populate index info structure */
		ii = populateIndexInfo(pcqCtx, tuple, indForm);
			
		/* construct hash key for the index */
		partIndexHashKey = constructIndexHashKey(partOid, rootOid, tuple, attmap, ii);

		/* lookup PartitionIndexHash table */
		partIndexHashEntry = (PartitionIndexHashEntry *)hash_search(PartitionIndexHash,
							(void *)partIndexHashKey,
							HASH_ENTER,
							&foundPartIndexHash);

		if (!foundPartIndexHash)
		{
			/* first time seeing this index */
			partIndexHashEntry->key = partIndexHashKey;

			/* assign a logical index id */
			partIndexHashEntry->logicalIndexId = logicalIndexId++;
		
			/* insert the entry into LogicalIndexInfoHash to keep track of logical indexes */
			logicalIndexInfoHashEntry = (LogicalIndexInfoHashEntry *)hash_search(LogicalIndexInfoHash,
								  (void *)&(partIndexHashEntry->logicalIndexId),
								  HASH_ENTER,
							 	  &foundLogicalIndexHash);


			if (foundLogicalIndexHash)
			{
				/* 
				 * we should not find the entry in the logical index hash as this is the first
				 * time we are seeing it
				 */
                		ereport(ERROR,
                                	(errcode(ERRCODE_INTERNAL_ERROR),
                                 	errmsg("error during BuildLogicalIndexInfo. Found indexrelid \"%d\" in hash",
                                                indForm->indexrelid
                                        )));
			}

			logicalIndexInfoHashEntry->logicalIndexId = partIndexHashEntry->logicalIndexId;
			logicalIndexInfoHashEntry->logicalIndexOid = indForm->indexrelid;
			logicalIndexInfoHashEntry->ii = ii;
			logicalIndexInfoHashEntry->partList = NIL;
			logicalIndexInfoHashEntry->defaultPartList = NIL;
		}
		else
		{
			/* we can release IndexInfo as we already have the information in the hash */
			if (ii->ii_Expressions)
				pfree(ii->ii_Expressions);
			if (ii->ii_Predicate)
				pfree(ii->ii_Predicate);
			pfree(ii);
		}


		/* update the PartitionIndexNode -> index bitmap */
		pNode->index = bms_add_member(pNode->index, partIndexHashEntry->logicalIndexId);
	}

	caql_endscan(pcqCtx);
}

/*
 * collapseIndexes
 *   Walk the PartitionNode tree 
 *	- For every node 
 *		- If a logical index exists on all the children of the node, record the index
 *	  	  on the node and wipeout the index from the children
 *	- recurse up the tree to the root.
 */
static bool
collapseIndexes(PartitionIndexNode **partitionIndexNode,
			LogicalIndexInfoHashEntry **entry)
{
	ListCell *lc, *lc1;
	PartitionIndexNode *pn = *partitionIndexNode;
	bool exists = TRUE;
	int lid= (*entry)->logicalIndexId;
	int childStatus = TRUE;

	if (pn->children)
	{
		foreach(lc, pn->children)
		{
			PartitionIndexNode *child = (PartitionIndexNode *) lfirst(lc);

			/* not a leaf, so check the children */
			exists = collapseIndexes(&child, entry);

			/* For every logical index we are traversing the WHOLE tree. */
			if (!exists)
			{
				/* there is a child that doesn't have the index */
				childStatus = FALSE;
			}
		}

		if (childStatus)
		{
			/* if index exists on every child, record in the parent */
			pn->index = bms_add_member(pn->index, lid);
			
			/* remove the index from the children */
			foreach (lc1, pn->children)
			{
				PartitionIndexNode *child = (PartitionIndexNode *) lfirst(lc1);
				bms_del_member(child->index, lid);
			}
		}
	}
	else
	{
		/* a leaf - check if index exists */
		if (!bms_is_member(lid, pn->index))
			exists = FALSE;
	}

	return exists;
}

/* 
 * indexParts
 *   Walks the tree of nodes and for each logical index that exists on the node,
 *   records the node (partition id) with the logical index in the hash.
 *   If the logical index exists on a default part, record some additional 
 *   information to indentify the specific default part.
 *
 *   This consolidates all the information regarding the logical index in the 
 *   hash table. As we exit from the function we have a hash table with an entry
 *   for every logical index
 * 	logical index id -> (logicalIndexOid, IndexInfo, partList, defaultPartList)
 */
static void indexParts(PartitionIndexNode **np, bool isDefault)
{
	LogicalIndexInfoHashEntry *entry;
	PartitionIndexNode *n = *np;
	bool found;

	if (n)
	{
		int x; 
		x = bms_first_from(n->index, 0);
		while (x >= 0)
		{
			entry = (LogicalIndexInfoHashEntry *)hash_search(LogicalIndexInfoHash,
								  (void *)&x,
								  HASH_FIND,
							 	  &found);
			if (!found)
			{
                		ereport(ERROR,
                                	(errcode(ERRCODE_INTERNAL_ERROR),
                                 	errmsg("error during BuildLogicalIndexInfo. Indexr not found \"%d\" in hash",
                                                x)));
			}

			if (n->isDefault || isDefault)
			{
				/* 
				 * We keep track of the default node in the PartitionIndexNode
				 * tree, since we need to output the defaultLevels information 
				 * to the caller.
				 */
				entry->defaultPartList = lappend(entry->defaultPartList, n);
				numIndexesOnDefaultParts++; 
			}
			else
				/* 	
				 * For regular non-default parts we just track the part oid
				 * which will be used to get the part constraint.
				 */ 
				entry->partList = lappend_oid(entry->partList, n->parchildrelid);
			x = bms_first_from(n->index, x + 1);
		}
	}

	if (n->children)
	{
		ListCell *lc;
		foreach (lc, n->children)
		{
			PartitionIndexNode *child = (PartitionIndexNode *) lfirst(lc);
			indexParts(&child, n->isDefault);
		}
	}
}

/*
 * getPartConstraints
 *   Given an OID, returns a Node that represents all the check constraints
 *   on the table AND'd together. 
 */
static Node *
getPartConstraints(Oid partOid, Oid rootOid)
{
	cqContext       *pcqCtx;
	cqContext       cqc;
	Relation        conRel;
	HeapTuple       conTup;
	Node            *conExpr;
	Node            *result = NULL;
	Datum           conBinDatum;
	char            *conBin;
	bool            conbinIsNull = false;
	char            conType = 'c';
	AttrMap		*map;

	/* create the map needed for mapping attnums */
	Relation rootRel = heap_open(rootOid, AccessShareLock);
	Relation partRel = heap_open(partOid, AccessShareLock);

	map_part_attrs(partRel, rootRel, &map, false); 

	heap_close(rootRel, AccessShareLock);
	heap_close(partRel, AccessShareLock);

	/* Fetch the pg_constraint row. */
	conRel = heap_open(ConstraintRelationId, AccessShareLock);

	pcqCtx = caql_beginscan(
			caql_addrel(cqclr(&cqc), conRel),
			cql("SELECT * FROM pg_constraint "
				" WHERE conrelid = :1 AND contype = :2",
				ObjectIdGetDatum(partOid), CharGetDatum(conType)));

	while (HeapTupleIsValid(conTup = caql_getnext(pcqCtx)))
	{
		/* Fetch the constraint expression in parsetree form */
		conBinDatum = heap_getattr(conTup, Anum_pg_constraint_conbin,
				RelationGetDescr(conRel), &conbinIsNull);

		Assert (!conbinIsNull);

		/* map the attnums in constraint expression to root attnums */
		conBin = TextDatumGetCString(conBinDatum);
		conExpr = stringToNode(conBin);
		conExpr = attrMapExpr(map, conExpr);

		if (result)
			result = (Node *)make_andclause(list_make2(result, conExpr));
		else
			result = conExpr;
	}

	caql_endscan(pcqCtx);
	heap_close(conRel, AccessShareLock);
	return result;
}

/*
 * populateIndexInfo
 *  Populate the IndexInfo structure with the information from pg_index tuple. 
 */
static IndexInfo *
populateIndexInfo(cqContext *pcqCtx,
			HeapTuple indTuple,
			Form_pg_index indForm)
{
	IndexInfo 	*ii;
	Datum		datum;
	bool		isnull = false;
	char		*str;

 	/*  
	 * We could just call existing BuildIndexInfo here. But BuildIndexInfo needs to open each index, 
 	 * and populate the relcache entry for the index relation. We can get the information by 
	 * just looking into pg_index tuple.
	 */
	ii = makeNode(IndexInfo);

	ii->ii_Unique = indForm->indisunique;
	ii->ii_NumIndexAttrs = indForm->indnatts;

	for (int i = 0; i < indForm->indnatts; i++)
	{
		ii->ii_KeyAttrNumbers[i] = indForm->indkey.values[i];
	}

	ii->ii_Expressions = NIL;
	datum = caql_getattr(pcqCtx, Anum_pg_index_indexprs, &isnull);
	if (!isnull)
	{
		str = TextDatumGetCString(datum);
		ii->ii_Expressions = (List *)stringToNode(str);
		pfree(str);
		fix_opfuncids((Node *) ii->ii_Expressions);
	}

	isnull = false;
	ii->ii_Predicate = NIL;
	datum = caql_getattr(pcqCtx, Anum_pg_index_indpred, &isnull);
	if (!isnull)
	{
		str = TextDatumGetCString(datum);
		ii->ii_Predicate = (List *)stringToNode(str);
		pfree(str);
		fix_opfuncids((Node *) ii->ii_Predicate);
	}

	return ii;
}

/* 
 * generateLogicalIndexPred
 *  generate the partial index predicate associated with each logical index.
 *  The predicate is generated by OR'ing the constraints of all parts on which the logical
 *  index exists.
 */
static void
generateLogicalIndexPred(LogicalIndexes *li,
				int *curIdx,
				LogicalIndexInfoHashEntry *entry,
				Oid root,
				int *numLogicalIndexes)
{
	Node            *conList;
	ListCell *lc; 
	AttrNumber	*indexKeys;

	if (list_length(entry->partList) > 0)
	{
		/* populate the index information */
		li->logicalIndexInfo[*curIdx]->partCons = NULL;
		li->logicalIndexInfo[*curIdx]->logicalIndexOid = entry->logicalIndexOid;
		li->logicalIndexInfo[*curIdx]->nColumns = entry->ii->ii_NumIndexAttrs;
		indexKeys = palloc(sizeof(AttrNumber) * entry->ii->ii_NumIndexAttrs);
		memcpy((char *)indexKeys,
				&(entry->ii->ii_KeyAttrNumbers), sizeof(AttrNumber) * entry->ii->ii_NumIndexAttrs);
		li->logicalIndexInfo[*curIdx]->indexKeys = indexKeys; 
		li->logicalIndexInfo[*curIdx]->indExprs = (List *)copyObject(entry->ii->ii_Expressions);
		li->logicalIndexInfo[*curIdx]->indPred = (List *)copyObject(entry->ii->ii_Predicate);
		li->logicalIndexInfo[*curIdx]->indIsUnique = entry->ii->ii_Unique;

		/* fetch the partList from the logical index hash entry */
		foreach(lc, entry->partList)
		{
			Oid partOid = lfirst_oid(lc);
	
			if (partOid != root)
			{	 
				/* fetch part constraint mapped to root */
				conList = getPartConstraints(partOid, root);
	
				/* OR them to current constraints */
				if (li->logicalIndexInfo[*curIdx]->partCons)
					li->logicalIndexInfo[*curIdx]->partCons = (Node *)
					make_orclause(list_make2(li->logicalIndexInfo[*curIdx]->partCons,
								conList));
				else
					li->logicalIndexInfo[*curIdx]->partCons = conList;
			}
		}

		(*curIdx)++;
		(*numLogicalIndexes)++;
	}

	/* from the hash entry, fetch the defaultPartList */
	foreach(lc, entry->defaultPartList)
	{
		PartitionIndexNode *node = (PartitionIndexNode *)lfirst(lc);
		AttrNumber *indexKeys;

		li->logicalIndexInfo[*curIdx]->partCons = NULL;
		li->logicalIndexInfo[*curIdx]->logicalIndexOid = entry->logicalIndexOid;
		li->logicalIndexInfo[*curIdx]->nColumns = entry->ii->ii_NumIndexAttrs;
		indexKeys = palloc(sizeof(AttrNumber) * entry->ii->ii_NumIndexAttrs);
		memcpy((char *)indexKeys,
			&(entry->ii->ii_KeyAttrNumbers), sizeof(AttrNumber) * entry->ii->ii_NumIndexAttrs);
		li->logicalIndexInfo[*curIdx]->indexKeys = indexKeys;
		li->logicalIndexInfo[*curIdx]->indExprs = (List *)copyObject(entry->ii->ii_Expressions);
		li->logicalIndexInfo[*curIdx]->indPred = (List *)copyObject(entry->ii->ii_Predicate);
		li->logicalIndexInfo[*curIdx]->indIsUnique = entry->ii->ii_Unique;

		/* 
		 * Default parts don't have constraints, so they cannot be represented
		 * as part of the partial index predicate.  
		 * 
		 * Instead each index on a default part is represented as a different logical
		 * index. Since we need a way to identify the specific default part, we include
		 * the defaultLevels information, in addition to ANY constraint on the default
		 * part.
		 */
		li->logicalIndexInfo[*curIdx]->partCons = getPartConstraints(node->parchildrelid, root);

		/* get the level on which partitioning key is default */
		li->logicalIndexInfo[*curIdx]->defaultLevels = list_copy(node->defaultLevels);
		(*curIdx)++;
		(*numLogicalIndexes)++;
	}
}

/*
 * createPartsIndexResult
 *  Populate the result array of logical indexes. Most of the information needed 
 *  is in the IndexInfoHash. The partial index predicate needs to be generated
 *  by a call to generateLogicalIndexPred.
 */
static LogicalIndexes *
createPartsIndexResult(Oid root)
{
	HASH_SEQ_STATUS hash_seq;
	int numResultRows = 0;
	LogicalIndexes *li;
	LogicalIndexInfoHashEntry *entry;

	hash_seq_init(&hash_seq, LogicalIndexInfoHash);

	/* 
	 * we allocate space for twice the number of logical indexes.
	 * this is because, every index on a default part will be
	 * returned as a separate index.
	 */
	numResultRows = numLogicalIndexes + numIndexesOnDefaultParts;

	li = (LogicalIndexes *) palloc0(sizeof(LogicalIndexes));

	li->numLogicalIndexes = 0;
	li->logicalIndexInfo = NULL;

	li->logicalIndexInfo = (LogicalIndexInfo **)palloc0(numResultRows * sizeof(LogicalIndexInfo *));

	/* should we limit the amount of memory being allocated here ? */
	for (int i=0; i < numResultRows; i++)
		li->logicalIndexInfo[i] = (LogicalIndexInfo *)palloc0(sizeof(LogicalIndexInfo));

	int curIdx = 0;

	/* for each logical index, get the part constraints as partial index predicates */
	while ((entry = hash_seq_search(&hash_seq)))
	{
		generateLogicalIndexPred(li, &curIdx, entry, root, &li->numLogicalIndexes);
	}

	return li;
}

/*
 * getPhysicalIndexRelid
 *   This function returns the physical index relid corresponding
 *   to the index described by the logicalIndexInfo input structure
 *   in the relation identified by the partOid.
 *   The 1st matching index is returned.
 * 
 * NOTE: index attnums in the provided logicalIndexInfo input structure
 * have to be mapped to the root.
 */
Oid
getPhysicalIndexRelid(LogicalIndexInfo *iInfo,
			Oid rootOid,
			Oid partOid)
{
	HeapTuple       tup;
	cqContext       cqc;
	cqContext       *pcqCtx;
	Oid             resultOid = InvalidOid;
	AttrNumber	*attMap = NULL;
	IndexInfo	*ii;
	Relation	indexRelation;
	bool		indKeyEqual;

	indexRelation = heap_open(IndexRelationId, AccessShareLock);

	/* get the attrmap */
	Relation rootRel = heap_open(rootOid, AccessShareLock);
	Relation partRel = heap_open(partOid, AccessShareLock);

	TupleDesc rootTupDesc = rootRel->rd_att;
	TupleDesc partTupDesc = partRel->rd_att;

	/* We don't handle the case if attnums are not mapped to the root */
	attMap = varattnos_map(rootTupDesc, partTupDesc);

	/* are we done here ? */
	heap_close(rootRel, AccessShareLock);
	heap_close(partRel, AccessShareLock);

	if (attMap)
	{
		/* map the attrnos if necessary */
		for (int i = 0; i < iInfo->nColumns; i++)
		{
			if (iInfo->indexKeys[i] != 0)
				iInfo->indexKeys[i] = attMap[(iInfo->indexKeys[i]) - 1];
		}

		/* map the attrnos in the indExprs and indPred */
		Expr *predExpr = (Expr *)iInfo->indPred;
		change_varattnos_of_a_node((Node *)predExpr, attMap);
		iInfo->indPred = (List *)predExpr;

		Expr *exprsExpr = (Expr *)iInfo->indExprs;
		change_varattnos_of_a_node((Node *)exprsExpr, attMap);
		iInfo->indExprs = (List *)exprsExpr;
	}

	/* now look up pg_index using indrelid */
	pcqCtx = caql_beginscan(
			caql_addrel(cqclr(&cqc), indexRelation),
			cql("SELECT * FROM pg_index "
				" WHERE indrelid = :1 ",
				ObjectIdGetDatum(partOid)));

	/* fetch tuples */
	while (HeapTupleIsValid(tup = caql_getnext(pcqCtx)))
	{
		Form_pg_index indForm = (Form_pg_index) GETSTRUCT(tup);

		ii = populateIndexInfo(pcqCtx, tup, indForm);

		if (ii->ii_NumIndexAttrs != iInfo->nColumns)
			continue;

		/* compare indKey */
		indKeyEqual = true;
		for (int i = 0; i < ii->ii_NumIndexAttrs; i++)
		{
			if (ii->ii_KeyAttrNumbers[i] != iInfo->indexKeys[i])
			{
				indKeyEqual = false;
				break;
			}
		}

		if (!indKeyEqual)
			continue;

		/* compare uniqueness attribute */
		if (iInfo->indIsUnique != ii->ii_Unique)
			continue;

		/* compare indPred */
		if (!equal(iInfo->indPred, ii->ii_Predicate))
			continue;

		/* compare indExprs */
		if (equal(iInfo->indExprs, ii->ii_Expressions))
		{
			/* found matching index */
			resultOid = indForm->indexrelid;
			break;
		}
	}

	caql_endscan(pcqCtx);
	heap_close(indexRelation, AccessShareLock);

	return resultOid;
}

/*
 * dumpPartsIndexInfo
 *   print debugging info
 */
static void
dumpPartsIndexInfo(PartitionIndexNode *n, int level)
{
	ListCell *lc; 
	StringInfoData logicalIndexes;
	initStringInfo(&logicalIndexes);

	if (n)
	{
		for (int i = 0; i <= level; i++)
			appendStringInfo(&logicalIndexes, "%s", "   ");

		appendStringInfo(&logicalIndexes, "%d ", n->parchildrelid);

		int x; 
		x = bms_first_from(n->index, 0);
		appendStringInfo(&logicalIndexes, "%s ", " (");

		while (x >= 0)
		{
			appendStringInfo(&logicalIndexes, "%d ", x);
			x = bms_first_from(n->index, x + 1);
		}

		appendStringInfo(&logicalIndexes, "%s ", " )");

		elog(DEBUG5, "%s", logicalIndexes.data);
	}


	if (n->children)
	{
		foreach (lc, n->children)
		{
			PartitionIndexNode *child = (PartitionIndexNode *) lfirst(lc);
			dumpPartsIndexInfo(child, level+1);
			appendStringInfo(&logicalIndexes, "     ");
		}
	}
}