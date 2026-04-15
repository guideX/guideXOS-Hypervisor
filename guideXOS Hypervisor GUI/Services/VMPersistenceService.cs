using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using guideXOS_Hypervisor_GUI.Models;

namespace guideXOS_Hypervisor_GUI.Services
{
    /// <summary>
    /// Service for persisting virtual machine configurations to disk
    /// </summary>
    public class VMPersistenceService
    {
        private static VMPersistenceService? _instance;
        private static readonly object _lock = new();
        private readonly string _vmStoragePath;
        private readonly JsonSerializerOptions _jsonOptions;

        private VMPersistenceService()
        {
            // Store VMs in a dedicated folder in the user's AppData directory
            var appDataPath = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            _vmStoragePath = Path.Combine(appDataPath, "guideXOS Hypervisor", "VirtualMachines");

            // Create directory if it doesn't exist
            if (!Directory.Exists(_vmStoragePath))
            {
                Directory.CreateDirectory(_vmStoragePath);
            }

            // Configure JSON serialization options for better readability
            _jsonOptions = new JsonSerializerOptions
            {
                WriteIndented = true,
                PropertyNameCaseInsensitive = true
            };
        }

        public static VMPersistenceService Instance
        {
            get
            {
                if (_instance == null)
                {
                    lock (_lock)
                    {
                        _instance ??= new VMPersistenceService();
                    }
                }
                return _instance;
            }
        }

        /// <summary>
        /// Get the storage path for VMs
        /// </summary>
        public string StoragePath => _vmStoragePath;

        /// <summary>
        /// Save a virtual machine configuration to disk
        /// </summary>
        public void SaveVM(VMStateModel vm)
        {
            try
            {
                var filePath = Path.Combine(_vmStoragePath, $"{vm.Id}.json");
                var json = JsonSerializer.Serialize(vm, _jsonOptions);
                File.WriteAllText(filePath, json);
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException($"Failed to save VM '{vm.Name}': {ex.Message}", ex);
            }
        }

        /// <summary>
        /// Save all virtual machines to disk
        /// </summary>
        public void SaveAllVMs(IEnumerable<VMStateModel> vms)
        {
            foreach (var vm in vms)
            {
                SaveVM(vm);
            }
        }

        /// <summary>
        /// Load a specific virtual machine from disk
        /// </summary>
        public VMStateModel? LoadVM(string vmId)
        {
            try
            {
                var filePath = Path.Combine(_vmStoragePath, $"{vmId}.json");
                
                if (!File.Exists(filePath))
                {
                    return null;
                }

                var json = File.ReadAllText(filePath);
                return JsonSerializer.Deserialize<VMStateModel>(json, _jsonOptions);
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException($"Failed to load VM with ID '{vmId}': {ex.Message}", ex);
            }
        }

        /// <summary>
        /// Load all virtual machines from disk
        /// </summary>
        public List<VMStateModel> LoadAllVMs()
        {
            var vms = new List<VMStateModel>();

            try
            {
                if (!Directory.Exists(_vmStoragePath))
                {
                    return vms;
                }

                var vmFiles = Directory.GetFiles(_vmStoragePath, "*.json");

                foreach (var filePath in vmFiles)
                {
                    try
                    {
                        var json = File.ReadAllText(filePath);
                        var vm = JsonSerializer.Deserialize<VMStateModel>(json, _jsonOptions);
                        
                        if (vm != null)
                        {
                            // Ensure VMs are loaded in PoweredOff state
                            vm.State = VMState.PoweredOff;
                            vms.Add(vm);
                        }
                    }
                    catch (Exception ex)
                    {
                        // Log error but continue loading other VMs
                        System.Diagnostics.Debug.WriteLine($"Error loading VM from {filePath}: {ex.Message}");
                    }
                }
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException($"Failed to load VMs: {ex.Message}", ex);
            }

            return vms;
        }

        /// <summary>
        /// Delete a virtual machine configuration from disk
        /// </summary>
        public void DeleteVM(string vmId)
        {
            try
            {
                var filePath = Path.Combine(_vmStoragePath, $"{vmId}.json");
                
                if (File.Exists(filePath))
                {
                    File.Delete(filePath);
                }
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException($"Failed to delete VM with ID '{vmId}': {ex.Message}", ex);
            }
        }

        /// <summary>
        /// Check if a VM exists on disk
        /// </summary>
        public bool VMExists(string vmId)
        {
            var filePath = Path.Combine(_vmStoragePath, $"{vmId}.json");
            return File.Exists(filePath);
        }

        /// <summary>
        /// Get the count of saved VMs
        /// </summary>
        public int GetVMCount()
        {
            if (!Directory.Exists(_vmStoragePath))
            {
                return 0;
            }

            return Directory.GetFiles(_vmStoragePath, "*.json").Length;
        }
    }
}
