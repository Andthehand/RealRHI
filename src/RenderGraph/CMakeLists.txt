# Graph compilation and resource pooling depend only on the RHI interfaces.
add_library(RealRHIGraph
    RenderGraph.h
    RenderGraph.cpp
    RenderGraphCompiler.cpp
    RenderGraphExecution.cpp
    RenderGraphQueuePlanner.cpp
    TransientResourcePool.h
    TransientResourcePool.cpp)
target_link_libraries(RealRHIGraph PUBLIC RealRHICore)
realrhi_configure_tracy(RealRHIGraph)
