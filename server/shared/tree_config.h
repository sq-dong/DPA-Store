#ifndef TREE_CONFIG_H
#define TREE_CONFIG_H

// Learned tree configuration
#define LEARNED_SEGMENTS_PER_INNER 7    // Maximum segments per inner node
#define LEARNED_KEYS_PER_INNER_SEGMENT 128 
#define LEARNED_KEYS_PER_LEAF 128         // Maximum sorted entries per leaf
#define LEARNED_INSERT_BUFFER_SIZE 16     // Max size of insert buffer (fits in 2 cache lines when keys separated)

// Load factors control how aggressively we try to keep leaves and inners below
// their physical capacity. Runtime rebuilds only apply the factor when a node
// overflows, while bulk load applies it unconditionally to leave extra slack.
// #define HOST_TREE_LEAF_RUNTIME_LOAD 0.25
// #define HOST_TREE_LEAF_BULK_LOAD    0.25
// #define HOST_TREE_INNER_RUNTIME_LOAD 0.25
// #define HOST_TREE_INNER_BULK_LOAD    0.25
#define HOST_TREE_LEAF_RUNTIME_LOAD 1.0
#define HOST_TREE_LEAF_BULK_LOAD    1.0
#define HOST_TREE_INNER_RUNTIME_LOAD 1.0
#define HOST_TREE_INNER_BULK_LOAD    1.0

// Epsilon values for learned index
#define EPSILON_BUILD_BULKLOAD_INNER 4       // Build epsilon for inner nodes during bulk load
#define EPSILON_BUILD_BULKLOAD_LEAF 8        // Build epsilon for leaf nodes during bulk load
#define EPSILON_BUILD_RUNTIME_INNER 4        // Build epsilon for inner nodes during runtime rebuilds
#define EPSILON_BUILD_RUNTIME_LEAF 8         // Build epsilon for leaf nodes during runtime rebuilds
#define EPSILON_TRAVERSE_INNER 4           // Traverse epsilon for inner nodes
#define EPSILON_TRAVERSE_LEAF 8           // Increased traverse epsilon for leaf nodes to handle larger prediction errors
#define EPSILON_BUILD_BULKLOAD_INNER 4       // Build epsilon for inner nodes during bulk load
#define EPSILON_BUILD_BULKLOAD_LEAF 8        // Build epsilon for leaf nodes during bulk load
#define EPSILON_BUILD_RUNTIME_INNER 4        // Build epsilon for inner nodes during runtime rebuilds
#define EPSILON_BUILD_RUNTIME_LEAF 8         // Build epsilon for leaf nodes during runtime rebuilds
#define EPSILON_TRAVERSE_INNER 4           // Traverse epsilon for inner nodes
#define EPSILON_TRAVERSE_LEAF 8           // Increased traverse epsilon for leaf nodes to handle larger prediction errors

// Fixed-point format
#define FIXED_POINT_SHIFT 64               // Number of bits for fractional part

// Tree parameters  
#define MAX_TREE_DEPTH 16           // Maximum tree depth

#endif // TREE_CONFIG_H