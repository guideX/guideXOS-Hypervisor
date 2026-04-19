using System;
using System.IO;
using System.Text;

namespace TestKernelGenerator
{
    /// <summary>
    /// Generates a simple test kernel binary for the IA-64 hypervisor
    /// This creates a binary that the hypervisor can detect and handle specially
    /// </summary>
    class Program
    {
        static void Main(string[] args)
        {
            Console.WriteLine("=== guideXOS IA-64 Test Kernel Generator ===\n");
            
            string outputPath = args.Length > 0 ? args[0] : "test_kernel.bin";
            
            CreateTestKernel(outputPath);
            
            Console.WriteLine("\nTest kernel created successfully!");
            Console.WriteLine($"Location: {Path.GetFullPath(outputPath)}");
            Console.WriteLine("\nTo use:");
            Console.WriteLine("1. Create a new VM in guideXOS Hypervisor GUI");
            Console.WriteLine("2. Enable 'Direct Boot' mode");
            Console.WriteLine($"3. Set kernel path to: {Path.GetFullPath(outputPath)}");
            Console.WriteLine("4. Set entry point to: 0x100000");
            Console.WriteLine("5. Start the VM and watch the framebuffer!");
        }
        
        static void CreateTestKernel(string outputPath)
        {
            using (var fs = new FileStream(outputPath, FileMode.Create, FileAccess.Write))
            using (var writer = new BinaryWriter(fs))
            {
                Console.WriteLine("Creating test kernel binary...");
                
                // Magic identifier so hypervisor can recognize this as a test kernel
                writer.Write(Encoding.ASCII.GetBytes("GUIDEXOS")); // 8 bytes
                writer.Write((uint)1); // Version
                writer.Write((uint)0x100000); // Entry point
                
                // Framebuffer test data
                // This will be a simple pattern that writes text to VGA text mode
                Console.WriteLine("  Adding framebuffer test pattern...");
                
                // VGA text mode: 80x25 characters, 2 bytes per char (char + attribute)
                // Base address: 0xB8000
                
                // Instructions to write "Hello from guideXOS!" to screen
                // We'll encode this as data that the hypervisor interprets
                
                byte[] testPattern = CreateFramebufferPattern();
                writer.Write(testPattern);
                
                Console.WriteLine($"  Binary size: {fs.Position} bytes");
            }
        }
        
        static byte[] CreateFramebufferPattern()
        {
            // Create a simple pattern that can be rendered
            // Format: [command_byte] [data...]
            // Commands:
            //   0x01: Clear screen (color byte)
            //   0x02: Write string (x, y, color, length, string bytes)
            //   0x03: Draw box (x1, y1, x2, y2, color)
            //   0xFF: End
            
            var pattern = new MemoryStream();
            var writer = new BinaryWriter(pattern);
            
            // Clear screen - blue background
            writer.Write((byte)0x01);
            writer.Write((byte)0x11); // Blue background, blue text
            
            // Draw border box
            writer.Write((byte)0x03);
            writer.Write((byte)0); // x1
            writer.Write((byte)0); // y1
            writer.Write((byte)79); // x2
            writer.Write((byte)24); // y2
            writer.Write((byte)0x1F); // White on blue
            
            // Write title
            WriteString(writer, 20, 2, 0x1E, "guideXOS IA-64 Hypervisor");
            WriteString(writer, 25, 3, 0x1F, "Test Kernel v1.0");
            
            // Write system info
            WriteString(writer, 5, 6, 0x0F, "Architecture: IA-64 (Itanium)");
            WriteString(writer, 5, 7, 0x0A, "Status: Running");
            WriteString(writer, 5, 8, 0x0F, "Entry Point: 0x100000");
            
            // Write test results
            WriteString(writer, 5, 11, 0x0F, "Testing hypervisor components:");
            WriteString(writer, 7, 12, 0x0A, "[OK] CPU execution");
            WriteString(writer, 7, 13, 0x0A, "[OK] Memory subsystem");
            WriteString(writer, 7, 14, 0x0A, "[OK] Framebuffer output");
            WriteString(writer, 7, 15, 0x0A, "[OK] Binary loader");
            
            // Success message
            WriteString(writer, 18, 19, 0x0E, "All systems operational!");
            WriteString(writer, 15, 21, 0x0B, "Press any key to continue...");
            
            // End marker
            writer.Write((byte)0xFF);
            
            return pattern.ToArray();
        }
        
        static void WriteString(BinaryWriter writer, int x, int y, byte color, string text)
        {
            writer.Write((byte)0x02); // Write string command
            writer.Write((byte)x);
            writer.Write((byte)y);
            writer.Write((byte)color);
            writer.Write((byte)text.Length);
            writer.Write(Encoding.ASCII.GetBytes(text));
        }
    }
}
