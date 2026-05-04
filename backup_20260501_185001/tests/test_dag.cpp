#include <gtest/gtest.h>
#include "core/Types.hpp"

// ==================== TaskGraph 单元测试 ====================

TEST(TaskGraphTest, AddNode) {
    clma::TaskGraph graph;
    EXPECT_EQ(graph.getNodeCount(), 0);
    
    clma::TaskNode node;
    node.id = "task_0";
    node.description = "Test task";
    graph.addNode(node);
    
    EXPECT_EQ(graph.getNodeCount(), 1);
    EXPECT_NE(graph.findNode("task_0"), nullptr);
    EXPECT_EQ(graph.findNode("task_0")->description, "Test task");
}

TEST(TaskGraphTest, FindNodeReturnsNullForUnknown) {
    const clma::TaskGraph graph;
    EXPECT_EQ(graph.findNode("nonexistent"), nullptr);
}

TEST(TaskGraphTest, ReadyNodesNoDependencies) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "task_0";
    n1.status = "pending";
    graph.addNode(n1);
    
    clma::TaskNode n2;
    n2.id = "task_1";
    n2.status = "pending";
    graph.addNode(n2);
    
    auto ready = graph.getReadyNodeIndices();
    EXPECT_EQ(ready.size(), 2);
}

TEST(TaskGraphTest, ReadyNodesRespectsDependencies) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "task_0";
    n1.status = "pending";
    graph.addNode(n1);
    
    clma::TaskNode n2;
    n2.id = "task_1";
    n2.status = "pending";
    n2.dependencies = {"task_0"};
    graph.addNode(n2);
    
    // task_0 pending → 只有 task_0 就绪
    auto ready = graph.getReadyNodeIndices();
    ASSERT_EQ(ready.size(), 1);
    EXPECT_EQ(graph.nodes[ready[0]].id, "task_0");
    
    // task_0 done → task_1 就绪
    graph.findNode("task_0")->status = "done";
    ready = graph.getReadyNodeIndices();
    ASSERT_EQ(ready.size(), 1);
    EXPECT_EQ(graph.nodes[ready[0]].id, "task_1");
}

TEST(TaskGraphTest, AllCompleted) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "t0";
    n1.status = "done";
    graph.addNode(n1);
    
    EXPECT_TRUE(graph.allCompleted());
    
    clma::TaskNode n2;
    n2.id = "t1";
    n2.status = "pending";
    graph.addNode(n2);
    
    EXPECT_FALSE(graph.allCompleted());
}

TEST(TaskGraphTest, CyclicDependency) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "t0";
    n1.dependencies = {"t1"};
    graph.addNode(n1);
    
    clma::TaskNode n2;
    n2.id = "t1";
    n2.dependencies = {"t0"};
    graph.addNode(n2);
    
    EXPECT_TRUE(graph.hasCyclicDependency());
}

TEST(TaskGraphTest, NoCyclicDependency) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "t0";
    graph.addNode(n1);
    
    clma::TaskNode n2;
    n2.id = "t1";
    n2.dependencies = {"t0"};
    graph.addNode(n2);
    
    clma::TaskNode n3;
    n3.id = "t2";
    n3.dependencies = {"t0", "t1"};
    graph.addNode(n3);
    
    EXPECT_FALSE(graph.hasCyclicDependency());
}

TEST(TaskGraphTest, CompletedAndFailedCount) {
    clma::TaskGraph graph;
    
    clma::TaskNode n1;
    n1.id = "t0";
    n1.status = "done";
    graph.addNode(n1);
    
    clma::TaskNode n2;
    n2.id = "t1";
    n2.status = "failed";
    graph.addNode(n2);
    
    clma::TaskNode n3;
    n3.id = "t2";
    n3.status = "pending";
    graph.addNode(n3);
    
    EXPECT_EQ(graph.getCompletedCount(), 1);
    EXPECT_EQ(graph.getFailedCount(), 1);
}

TEST(TaskGraphTest, RetryOnFailedNode) {
    clma::TaskGraph graph;
    
    // task_0 done, task_1 failed but can retry
    clma::TaskNode n0;
    n0.id = "t0";
    n0.status = "done";
    graph.addNode(n0);
    
    clma::TaskNode n1;
    n1.id = "t1";
    n1.status = "failed";
    n1.retry_count = 1;
    n1.max_retries = 2;
    n1.dependencies = {"t0"};
    graph.addNode(n1);
    
    // t1 failed but retry_count < max_retries AND deps done → should be ready
    auto ready = graph.getReadyNodeIndices();
    ASSERT_EQ(ready.size(), 1);
    EXPECT_EQ(graph.nodes[ready[0]].id, "t1");
}

TEST(TaskGraphTest, ExhaustedRetryNotReady) {
    clma::TaskGraph graph;
    
    clma::TaskNode n0;
    n0.id = "t0";
    n0.status = "done";
    graph.addNode(n0);
    
    clma::TaskNode n1;
    n1.id = "t1";
    n1.status = "failed";
    n1.retry_count = 2;
    n1.max_retries = 2;
    n1.dependencies = {"t0"};
    graph.addNode(n1);
    
    // retries exhausted → not ready
    auto ready = graph.getReadyNodeIndices();
    EXPECT_EQ(ready.size(), 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
