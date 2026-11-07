#include "preprocessor_defaults.slangh"
import preprocessor_import;

#ifdef EMPTY_REPLACEMENT
EMPTY_REPLACEMENT
#endif

[shader("compute")]
[numthreads(GROUP_SIZE, featureGroupSize, 1)]
void computeMain() {}
