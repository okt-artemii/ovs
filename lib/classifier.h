/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CLASSIFIER_H
#define CLASSIFIER_H 1

/* Flow classifier.
 *
 *
 * What?
 * =====
 *
 * A flow classifier holds any number of "rules", each of which specifies
 * values to match for some fields or subfields and a priority.  Each OpenFlow
 * table is implemented as a flow classifier.
 *
 * The classifier has two primary design goals.  The first is obvious: given a
 * set of packet headers, as quickly as possible find the highest-priority rule
 * that matches those headers.  The following section describes the second
 * goal.
 *
 *
 * "Un-wildcarding"
 * ================
 *
 * A primary goal of the flow classifier is to produce, as a side effect of a
 * packet lookup, a wildcard mask that indicates which bits of the packet
 * headers were essential to the classification result.  Ideally, a 1-bit in
 * any position of this mask means that, if the corresponding bit in the packet
 * header were flipped, then the classification result might change.  A 0-bit
 * means that changing the packet header bit would have no effect.  Thus, the
 * wildcarded bits are the ones that played no role in the classification
 * decision.
 *
 * Such a wildcard mask is useful with datapaths that support installing flows
 * that wildcard fields or subfields.  If an OpenFlow lookup for a TCP flow
 * does not actually look at the TCP source or destination ports, for example,
 * then the switch may install into the datapath a flow that wildcards the port
 * numbers, which in turn allows the datapath to handle packets that arrive for
 * other TCP source or destination ports without additional help from
 * ovs-vswitchd.  This is useful for the Open vSwitch software and,
 * potentially, for ASIC-based switches as well.
 *
 * Some properties of the wildcard mask:
 *
 *     - "False 1-bits" are acceptable, that is, setting a bit in the wildcard
 *       mask to 1 will never cause a packet to be forwarded the wrong way.
 *       As a corollary, a wildcard mask composed of all 1-bits will always
 *       yield correct (but often needlessly inefficient) behavior.
 *
 *     - "False 0-bits" can cause problems, so they must be avoided.  In the
 *       extreme case, a mask of all 0-bits is only correct if the classifier
 *       contains only a single flow that matches all packets.
 *
 *     - 0-bits are desirable because they allow the datapath to act more
 *       autonomously, relying less on ovs-vswitchd to process flow setups,
 *       thereby improving performance.
 *
 *     - We don't know a good way to generate wildcard masks with the maximum
 *       (correct) number of 0-bits.  We use various approximations, described
 *       in later sections.
 *
 *     - Wildcard masks for lookups in a given classifier yield a
 *       non-overlapping set of rules.  More specifically:
 *
 *       Consider an classifier C1 filled with an arbitrary collection of rules
 *       and an empty classifier C2.  Now take a set of packet headers H and
 *       look it up in C1, yielding a highest-priority matching rule R1 and
 *       wildcard mask M.  Form a new classifier rule R2 out of packet headers
 *       H and mask M, and add R2 to C2 with a fixed priority.  If one were to
 *       do this for every possible set of packet headers H, then this
 *       process would not attempt to add any overlapping rules to C2, that is,
 *       any packet lookup using the rules generated by this process matches at
 *       most one rule in C2.
 *
 * During the lookup process, the classifier starts out with a wildcard mask
 * that is all 0-bits, that is, fully wildcarded.  As lookup proceeds, each
 * step tends to add constraints to the wildcard mask, that is, change
 * wildcarded 0-bits into exact-match 1-bits.  We call this "un-wildcarding".
 * A lookup step that examines a particular field must un-wildcard that field.
 * In general, un-wildcarding is necessary for correctness but undesirable for
 * performance.
 *
 *
 * Basic Classifier Design
 * =======================
 *
 * Suppose that all the rules in a classifier had the same form.  For example,
 * suppose that they all matched on the source and destination Ethernet address
 * and wildcarded all the other fields.  Then the obvious way to implement a
 * classifier would be a hash table on the source and destination Ethernet
 * addresses.  If new classification rules came along with a different form,
 * you could add a second hash table that hashed on the fields matched in those
 * rules.  With two hash tables, you look up a given flow in each hash table.
 * If there are no matches, the classifier didn't contain a match; if you find
 * a match in one of them, that's the result; if you find a match in both of
 * them, then the result is the rule with the higher priority.
 *
 * This is how the classifier works.  In a "struct classifier", each form of
 * "struct cls_rule" present (based on its ->match.mask) goes into a separate
 * "struct cls_subtable".  A lookup does a hash lookup in every "struct
 * cls_subtable" in the classifier and tracks the highest-priority match that
 * it finds.  The subtables are kept in a descending priority order according
 * to the highest priority rule in each subtable, which allows lookup to skip
 * over subtables that can't possibly have a higher-priority match than already
 * found.  Eliminating lookups through priority ordering aids both classifier
 * primary design goals: skipping lookups saves time and avoids un-wildcarding
 * fields that those lookups would have examined.
 *
 * One detail: a classifier can contain multiple rules that are identical other
 * than their priority.  When this happens, only the highest priority rule out
 * of a group of otherwise identical rules is stored directly in the "struct
 * cls_subtable", with the other almost-identical rules chained off a linked
 * list inside that highest-priority rule.
 *
 *
 * Staged Lookup (Wildcard Optimization)
 * =====================================
 *
 * Subtable lookup is performed in ranges defined for struct flow, starting
 * from metadata (registers, in_port, etc.), then L2 header, L3, and finally
 * L4 ports.  Whenever it is found that there are no matches in the current
 * subtable, the rest of the subtable can be skipped.
 *
 * Staged lookup does not reduce lookup time, and it may increase it, because
 * it changes a single hash table lookup into multiple hash table lookups.
 * It reduces un-wildcarding significantly in important use cases.
 *
 *
 * Prefix Tracking (Wildcard Optimization)
 * =======================================
 *
 * Classifier uses prefix trees ("tries") for tracking the used
 * address space, enabling skipping classifier tables containing
 * longer masks than necessary for the given address.  This reduces
 * un-wildcarding for datapath flows in parts of the address space
 * without host routes, but consulting extra data structures (the
 * tries) may slightly increase lookup time.
 *
 * Trie lookup is interwoven with staged lookup, so that a trie is
 * searched only when the configured trie field becomes relevant for
 * the lookup.  The trie lookup results are retained so that each trie
 * is checked at most once for each classifier lookup.
 *
 * This implementation tracks the number of rules at each address
 * prefix for the whole classifier.  More aggressive table skipping
 * would be possible by maintaining lists of tables that have prefixes
 * at the lengths encountered on tree traversal, or by maintaining
 * separate tries for subsets of rules separated by metadata fields.
 *
 * Prefix tracking is configured via OVSDB "Flow_Table" table,
 * "fieldspec" column.  "fieldspec" is a string map where a "prefix"
 * key tells which fields should be used for prefix tracking.  The
 * value of the "prefix" key is a comma separated list of field names.
 *
 * There is a maximum number of fields that can be enabled for any one
 * flow table.  Currently this limit is 3.
 *
 *
 * Partitioning (Lookup Time and Wildcard Optimization)
 * ====================================================
 *
 * Suppose that a given classifier is being used to handle multiple stages in a
 * pipeline using "resubmit", with metadata (that is, the OpenFlow 1.1+ field
 * named "metadata") distinguishing between the different stages.  For example,
 * metadata value 1 might identify ingress rules, metadata value 2 might
 * identify ACLs, and metadata value 3 might identify egress rules.  Such a
 * classifier is essentially partitioned into multiple sub-classifiers on the
 * basis of the metadata value.
 *
 * The classifier has a special optimization to speed up matching in this
 * scenario:
 *
 *     - Each cls_subtable that matches on metadata gets a tag derived from the
 *       subtable's mask, so that it is likely that each subtable has a unique
 *       tag.  (Duplicate tags have a performance cost but do not affect
 *       correctness.)
 *
 *     - For each metadata value matched by any cls_rule, the classifier
 *       constructs a "struct cls_partition" indexed by the metadata value.
 *       The cls_partition has a 'tags' member whose value is the bitwise-OR of
 *       the tags of each cls_subtable that contains any rule that matches on
 *       the cls_partition's metadata value.  In other words, struct
 *       cls_partition associates metadata values with subtables that need to
 *       be checked with flows with that specific metadata value.
 *
 * Thus, a flow lookup can start by looking up the partition associated with
 * the flow's metadata, and then skip over any cls_subtable whose 'tag' does
 * not intersect the partition's 'tags'.  (The flow must also be looked up in
 * any cls_subtable that doesn't match on metadata.  We handle that by giving
 * any such cls_subtable TAG_ALL as its 'tags' so that it matches any tag.)
 *
 * Partitioning saves lookup time by reducing the number of subtable lookups.
 * Each eliminated subtable lookup also reduces the amount of un-wildcarding.
 *
 *
 * Thread-safety
 * =============
 *
 * The classifier may safely be accessed by many reader threads concurrently or
 * by a single writer. */

#include "cmap.h"
#include "match.h"
#include "meta-flow.h"
#include "ovs-thread.h"
#include "pvector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Classifier internal data structures. */
struct cls_subtable;
struct cls_match;

struct trie_node;
typedef OVSRCU_TYPE(struct trie_node *) rcu_trie_ptr;

/* Prefix trie for a 'field' */
struct cls_trie {
    const struct mf_field *field; /* Trie field, or NULL. */
    rcu_trie_ptr root;            /* NULL if none. */
};

enum {
    CLS_MAX_INDICES = 3,   /* Maximum number of lookup indices per subtable. */
    CLS_MAX_TRIES = 3      /* Maximum number of prefix trees per classifier. */
};

/* A flow classifier. */
struct classifier {
    struct ovs_mutex mutex;
    int n_rules OVS_GUARDED;        /* Total number of rules. */
    uint8_t n_flow_segments;
    uint8_t flow_segments[CLS_MAX_INDICES]; /* Flow segment boundaries to use
                                             * for staged lookup. */
    struct cmap subtables_map;      /* Contains "struct cls_subtable"s.  */
    struct pvector subtables;
    struct cmap partitions;         /* Contains "struct cls_partition"s. */
    struct cls_trie tries[CLS_MAX_TRIES]; /* Prefix tries. */
    unsigned int n_tries;
};

/* A rule to be inserted to the classifier. */
struct cls_rule {
    struct minimatch match;      /* Matching rule. */
    unsigned int priority;       /* Larger numbers are higher priorities. */
    struct cls_match *cls_match; /* NULL if rule is not in a classifier. */
};

void cls_rule_init(struct cls_rule *, const struct match *,
                   unsigned int priority);
void cls_rule_init_from_minimatch(struct cls_rule *, const struct minimatch *,
                                  unsigned int priority);
void cls_rule_clone(struct cls_rule *, const struct cls_rule *);
void cls_rule_move(struct cls_rule *dst, struct cls_rule *src);
void cls_rule_destroy(struct cls_rule *);

bool cls_rule_equal(const struct cls_rule *, const struct cls_rule *);
uint32_t cls_rule_hash(const struct cls_rule *, uint32_t basis);

void cls_rule_format(const struct cls_rule *, struct ds *);

bool cls_rule_is_catchall(const struct cls_rule *);

bool cls_rule_is_loose_match(const struct cls_rule *rule,
                             const struct minimatch *criteria);

void classifier_init(struct classifier *, const uint8_t *flow_segments);
void classifier_destroy(struct classifier *);
bool classifier_set_prefix_fields(struct classifier *,
                                  const enum mf_field_id *trie_fields,
                                  unsigned int n_trie_fields);

bool classifier_is_empty(const struct classifier *);
int classifier_count(const struct classifier *);
void classifier_insert(struct classifier *, struct cls_rule *);
struct cls_rule *classifier_replace(struct classifier *, struct cls_rule *);

struct cls_rule *classifier_remove(struct classifier *, struct cls_rule *);
struct cls_rule *classifier_lookup(const struct classifier *,
                                   const struct flow *,
                                   struct flow_wildcards *);
bool classifier_lookup_miniflow_batch(const struct classifier *cls,
                                      const struct miniflow **flows,
                                      struct cls_rule **rules,
                                      const size_t cnt);
enum { CLASSIFIER_MAX_BATCH = 256 };
bool classifier_rule_overlaps(const struct classifier *,
                              const struct cls_rule *);

struct cls_rule *classifier_find_rule_exactly(const struct classifier *,
                                              const struct cls_rule *);

struct cls_rule *classifier_find_match_exactly(const struct classifier *,
                                               const struct match *,
                                               unsigned int priority);

/* Iteration. */

struct cls_cursor {
    const struct classifier *cls;
    const struct cls_subtable *subtable;
    const struct cls_rule *target;
    struct cmap_cursor subtables;
    struct cmap_cursor rules;
    struct cls_rule *rule;
    bool safe;
};

/* Iteration requires mutual exclusion of writers.  We do this by taking
 * a mutex for the duration of the iteration, except for the
 * 'SAFE' variant, where we release the mutex for the body of the loop. */
struct cls_cursor cls_cursor_start(const struct classifier *cls,
                                   const struct cls_rule *target,
                                   bool safe);

void cls_cursor_advance(struct cls_cursor *);

#define CLS_FOR_EACH(RULE, MEMBER, CLS) \
    CLS_FOR_EACH_TARGET(RULE, MEMBER, CLS, NULL)
#define CLS_FOR_EACH_TARGET(RULE, MEMBER, CLS, TARGET)                  \
    for (struct cls_cursor cursor__ = cls_cursor_start(CLS, TARGET, false); \
         (cursor__.rule                                                 \
          ? (INIT_CONTAINER(RULE, cursor__.rule, MEMBER),               \
             true)                                                      \
          : false);                                                     \
         cls_cursor_advance(&cursor__))

/* These forms allows classifier_remove() to be called within the loop. */
#define CLS_FOR_EACH_SAFE(RULE, MEMBER, CLS) \
    CLS_FOR_EACH_TARGET_SAFE(RULE, MEMBER, CLS, NULL)
#define CLS_FOR_EACH_TARGET_SAFE(RULE, MEMBER, CLS, TARGET)             \
    for (struct cls_cursor cursor__ = cls_cursor_start(CLS, TARGET, true); \
         (cursor__.rule                                                 \
          ? (INIT_CONTAINER(RULE, cursor__.rule, MEMBER),               \
             cls_cursor_advance(&cursor__),                             \
             true)                                                      \
          : false);                                                     \
        )                                                               \

#ifdef __cplusplus
}
#endif

#endif /* classifier.h */
