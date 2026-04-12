const std = @import("std");
const Allocator = std.mem.Allocator;

pub fn BinarySearchTree(comptime T: type) type {
    return struct {
        const Self = @This();
        
        const Node = struct {
            data: T,
            left: ?*Node,
            right: ?*Node,
        };
        
        root: ?*Node,
        allocator: Allocator,
        
        pub fn init(allocator: Allocator) Self {
            return Self{
                .root = null,
                .allocator = allocator,
            };
        }
        
        pub fn deinit(self: *Self) void {
            self.destroyNode(self.root);
        }
        
        fn destroyNode(self: *Self, node: ?*Node) void {
            if (node) |n| {
                self.destroyNode(n.left);
                self.destroyNode(n.right);
                self.allocator.destroy(n);
            }
        }
        
        pub fn insert(self: *Self, value: T) !void {
            self.root = try self.insertNode(self.root, value);
        }
        
        fn insertNode(self: *Self, node: ?*Node, value: T) !?*Node {
            if (node == null) {
                const new_node = try self.allocator.create(Node);
                new_node.* = Node{
                    .data = value,
                    .left = null,
                    .right = null,
                };
                return new_node;
            }
            
            const n = node.?;
            if (value < n.data) {
                n.left = try self.insertNode(n.left, value);
            } else if (value > n.data) {
                n.right = try self.insertNode(n.right, value);
            }
            
            return node;
        }
        
        pub fn search(self: *Self, value: T) bool {
            return self.searchNode(self.root, value);
        }
        
        fn searchNode(self: *Self, node: ?*Node, value: T) bool {
            _ = self;
            if (node == null) return false;
            
            const n = node.?;
            if (value == n.data) return true;
            if (value < n.data) return self.searchNode(n.left, value);
            return self.searchNode(n.right, value);
        }
        
        pub fn delete(self: *Self, value: T) void {
            self.root = self.deleteNode(self.root, value);
        }
        
        fn deleteNode(self: *Self, node: ?*Node, value: T) ?*Node {
            if (node == null) return null;
            
            const n = node.?;
            if (value < n.data) {
                n.left = self.deleteNode(n.left, value);
            } else if (value > n.data) {
                n.right = self.deleteNode(n.right, value);
            } else {
                // Node to delete found
                if (n.left == null) {
                    const temp = n.right;
                    self.allocator.destroy(n);
                    return temp;
                } else if (n.right == null) {
                    const temp = n.left;
                    self.allocator.destroy(n);
                    return temp;
                }
                
                // Node has two children
                const successor = self.findMin(n.right.?);
                n.data = successor.data;
                n.right = self.deleteNode(n.right, successor.data);
            }
            
            return node;
        }
        
        fn findMin(self: *Self, node: *Node) *Node {
            _ = self;
            var current = node;
            while (current.left != null) {
                current = current.left.?;
            }
            return current;
        }
        
        pub fn inorderTraversal(self: *Self, writer: anytype) !void {
            try self.inorderNode(self.root, writer);
        }
        
        fn inorderNode(self: *Self, node: ?*Node, writer: anytype) !void {
            if (node) |n| {
                try self.inorderNode(n.left, writer);
                try writer.print("{} ", .{n.data});
                try self.inorderNode(n.right, writer);
            }
        }
    };
}

// Example usage
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();
    
    var bst = BinarySearchTree(i32).init(allocator);
    defer bst.deinit();
    
    try bst.insert(50);
    try bst.insert(30);
    try bst.insert(70);
    try bst.insert(20);
    try bst.insert(40);
    try bst.insert(60);
    try bst.insert(80);
    
    const stdout = std.io.getStdOut().writer();
    try stdout.print("Inorder traversal: ");
    try bst.inorderTraversal(stdout);
    try stdout.print("\n");
    
    try stdout.print("Search 40: {}\n", .{bst.search(40)});
    try stdout.print("Search 25: {}\n", .{bst.search(25)});
    
    bst.delete(30);
    try stdout.print("After deleting 30: ");
    try bst.inorderTraversal(stdout);
    try stdout.print("\n");
}
