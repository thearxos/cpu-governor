#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_PATH 256
#define MAX_BUFFER 1024
#define INSTALL_PATH "/usr/local/bin/cpu-governor"
#define SERVICE_PATH "/etc/systemd/system/cpu-performance.service"
#define SYSCTL_CONF "/etc/sysctl.d/99-cpu-performance.conf"

// ANSI color codes
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"

typedef struct {
    char name[32];
    char description[64];
} governor_info;

static const governor_info governors[] = {
    {"performance", "Maximum performance, highest frequencies"},
    {"powersave", "Power saving, lowest frequencies"},
    {"ondemand", "Dynamic scaling based on CPU load"},
    {"conservative", "Conservative frequency scaling"},
    {"schedutil", "Scheduler-guided frequency scaling"},
    {"userspace", "User-controlled frequency scaling"}
};

static const int num_governors = sizeof(governors) / sizeof(governor_info);

void print_colored(const char* color, const char* prefix, const char* message) {
    printf("%s[%s]%s %s\n", color, prefix, RESET, message);
}

int is_root() {
    return getuid() == 0;
}

int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

int read_file(const char* path, char* buffer, size_t size) {
    FILE* file = fopen(path, "r");
    if (!file) return 0;
    
    if (fgets(buffer, size, file)) {
        char* newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

int write_file(const char* path, const char* value) {
    FILE* file = fopen(path, "w");
    if (!file) return 0;
    
    int result = fprintf(file, "%s", value) > 0;
    fclose(file);
    return result;
}

int get_cpu_count() {
    glob_t glob_result;
    int count = 0;
    
    if (glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor", 
             GLOB_NOSORT, NULL, &glob_result) == 0) {
        count = glob_result.gl_pathc;
        globfree(&glob_result);
    }
    return count;
}

void detect_cpu_type(char* vendor, size_t size) {
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        strncpy(vendor, "unknown", size);
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strncmp(line, "vendor_id", 9) == 0) {
            if (strstr(line, "Intel")) {
                strncpy(vendor, "intel", size);
            } else if (strstr(line, "AMD")) {
                strncpy(vendor, "amd", size);
            } else {
                strncpy(vendor, "other", size);
            }
            fclose(cpuinfo);
            return;
        }
    }
    fclose(cpuinfo);
    strncpy(vendor, "unknown", size);
}

void show_current_status() {
    char buffer[MAX_BUFFER];
    char available[MAX_BUFFER];
    char vendor[32];
    
    printf("%s=== CPU Governor Status ===%s\n", CYAN, RESET);
    
    detect_cpu_type(vendor, sizeof(vendor));
    printf("CPU Vendor: %s%s%s\n", YELLOW, vendor, RESET);
    
    if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buffer, sizeof(buffer))) {
        printf("Current Governor: %s%s%s\n", GREEN, buffer, RESET);
    } else {
        print_colored(RED, "ERROR", "Cannot read current governor");
        return;
    }
    
    if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", available, sizeof(available))) {
        printf("Available: %s\n", available);
    }
    
    int cpu_count = get_cpu_count();
    printf("CPU Cores: %d\n", cpu_count);
    
    // Check turbo/boost status
    if (file_exists("/sys/devices/system/cpu/intel_pstate/no_turbo")) {
        read_file("/sys/devices/system/cpu/intel_pstate/no_turbo", buffer, sizeof(buffer));
        printf("Intel Turbo: %s%s%s\n", 
               strcmp(buffer, "0") == 0 ? GREEN : RED,
               strcmp(buffer, "0") == 0 ? "ENABLED" : "DISABLED",
               RESET);
    } else if (file_exists("/sys/devices/system/cpu/cpufreq/boost")) {
        read_file("/sys/devices/system/cpu/cpufreq/boost", buffer, sizeof(buffer));
        printf("CPU Boost: %s%s%s\n",
               strcmp(buffer, "1") == 0 ? GREEN : RED,
               strcmp(buffer, "1") == 0 ? "ENABLED" : "DISABLED",
               RESET);
    }
    
    // Show frequency range
    if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", buffer, sizeof(buffer))) {
        int min_freq = atoi(buffer) / 1000;
        if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", buffer, sizeof(buffer))) {
            int max_freq = atoi(buffer) / 1000;
            printf("Frequency Range: %d - %d MHz\n", min_freq, max_freq);
        }
    }
    
    printf("Current Frequencies (MHz): ");
    for (int i = 0; i < 8 && i < cpu_count; i++) {
        char freq_path[MAX_PATH];
        snprintf(freq_path, sizeof(freq_path), 
                "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        
        if (read_file(freq_path, buffer, sizeof(buffer))) {
            int freq_mhz = atoi(buffer) / 1000;
            printf("CPU%d:%d ", i, freq_mhz);
        }
    }
    if (cpu_count > 8) printf("...");
    printf("\n");
}

int validate_governor(const char* governor) {
    char available[MAX_BUFFER];
    
    if (!read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", 
                   available, sizeof(available))) {
        print_colored(RED, "ERROR", "Cannot read available governors");
        return 0;
    }
    
    return strstr(available, governor) != NULL;
}

int set_governor(const char* governor) {
    glob_t glob_result;
    int success_count = 0;
    int total_count = 0;
    
    if (!validate_governor(governor)) {
        printf("%s[ERROR]%s Governor '%s' not available\n", RED, RESET, governor);
        return 0;
    }
    
    printf("%s[INFO]%s Setting governor to: %s\n", BLUE, RESET, governor);
    
    if (glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor", 
             GLOB_NOSORT, NULL, &glob_result) == 0) {
        
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            if (write_file(glob_result.gl_pathv[i], governor)) {
                success_count++;
            }
            total_count++;
        }
        globfree(&glob_result);
    }
    
    if (success_count == total_count && total_count > 0) {
        printf("%s[SUCCESS]%s Applied to %d CPU cores\n", GREEN, RESET, total_count);
        return 1;
    } else {
        printf("%s[ERROR]%s Failed on some cores (%d/%d)\n", 
               RED, RESET, success_count, total_count);
        return 0;
    }
}

void set_cpu_boost(int enable) {
    int success = 0;
    
    // Try Intel P-State (inverted logic)
    if (file_exists("/sys/devices/system/cpu/intel_pstate/no_turbo")) {
        const char* value = enable ? "0" : "1";
        if (write_file("/sys/devices/system/cpu/intel_pstate/no_turbo", value)) {
            printf("%s[INFO]%s Intel Turbo Boost %s\n", 
                   BLUE, RESET, enable ? "enabled" : "disabled");
            success = 1;
        }
    }
    
    // Try generic ACPI boost
    if (file_exists("/sys/devices/system/cpu/cpufreq/boost")) {
        const char* value = enable ? "1" : "0";
        if (write_file("/sys/devices/system/cpu/cpufreq/boost", value)) {
            printf("%s[INFO]%s CPU Boost %s\n", 
                   BLUE, RESET, enable ? "enabled" : "disabled");
            success = 1;
        }
    }
    
    // Try AMD-specific
    if (file_exists("/sys/devices/system/cpu/cpufreq/amd_pstate/boost")) {
        const char* value = enable ? "1" : "0";
        if (write_file("/sys/devices/system/cpu/cpufreq/amd_pstate/boost", value)) {
            printf("%s[INFO]%s AMD Boost %s\n", 
                   BLUE, RESET, enable ? "enabled" : "disabled");
            success = 1;
        }
    }
    
    if (!success) {
        printf("%s[WARN]%s CPU boost control not available\n", YELLOW, RESET);
    }
}

void set_energy_preference(const char* preference) {
    glob_t glob_result;
    
    if (glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/energy_performance_preference", 
             GLOB_NOSORT, NULL, &glob_result) == 0) {
        
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            write_file(glob_result.gl_pathv[i], preference);
        }
        globfree(&glob_result);
        printf("%s[INFO]%s Energy preference set to: %s\n", BLUE, RESET, preference);
    }
}

void apply_kernel_optimizations() {
    // Disable CPU idle states for maximum performance
    if (file_exists("/sys/devices/system/cpu/cpu0/cpuidle/state1/disable")) {
        system("for state in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do echo 1 > $state 2>/dev/null; done");
        printf("%s[INFO]%s CPU idle states disabled\n", BLUE, RESET);
    }
    
    // Set maximum CPU performance for Intel P-State
    if (file_exists("/sys/devices/system/cpu/intel_pstate/max_perf_pct")) {
        write_file("/sys/devices/system/cpu/intel_pstate/max_perf_pct", "100");
        write_file("/sys/devices/system/cpu/intel_pstate/min_perf_pct", "100");
        printf("%s[INFO]%s Intel P-State set to 100%% performance\n", BLUE, RESET);
    }
}

void restore_kernel_defaults() {
    // Re-enable CPU idle states
    if (file_exists("/sys/devices/system/cpu/cpu0/cpuidle/state1/disable")) {
        system("for state in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do echo 0 > $state 2>/dev/null; done");
        printf("%s[INFO]%s CPU idle states enabled\n", BLUE, RESET);
    }
    
    // Reset Intel P-State to defaults
    if (file_exists("/sys/devices/system/cpu/intel_pstate/max_perf_pct")) {
        write_file("/sys/devices/system/cpu/intel_pstate/max_perf_pct", "100");
        write_file("/sys/devices/system/cpu/intel_pstate/min_perf_pct", "0");
        printf("%s[INFO]%s Intel P-State reset to defaults\n", BLUE, RESET);
    }
}

void performance_mode() {
    printf("%s=== MAXIMUM PERFORMANCE MODE ===%s\n", MAGENTA, RESET);
    set_governor("performance");
    set_cpu_boost(1);
    set_energy_preference("performance");
    apply_kernel_optimizations();
    printf("%s[SUCCESS]%s System configured for maximum performance\n", GREEN, RESET);
}

void powersave_mode() {
    printf("%s=== POWER SAVING MODE ===%s\n", MAGENTA, RESET);
    set_governor("powersave");
    set_cpu_boost(0);
    set_energy_preference("power");
    restore_kernel_defaults();
    printf("%s[SUCCESS]%s System configured for power saving\n", GREEN, RESET);
}

int create_systemd_service() {
    FILE* service = fopen(SERVICE_PATH, "w");
    if (!service) {
        print_colored(RED, "ERROR", "Cannot create systemd service");
        return 0;
    }
    
    fprintf(service,
        "[Unit]\n"
        "Description=CPU Performance Mode\n"
        "After=multi-user.target\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=%s performance\n"
        "RemainAfterExit=yes\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        INSTALL_PATH);
    
    fclose(service);
    
    system("systemctl daemon-reload");
    system("systemctl enable cpu-performance.service");
    
    printf("%s[SUCCESS]%s Systemd service created and enabled\n", GREEN, RESET);
    printf("Performance mode will apply on every boot\n");
    return 1;
}

int install_systemwide() {
    char exe_path[MAX_PATH];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    
    if (len == -1) {
        print_colored(RED, "ERROR", "Cannot determine executable path");
        return 0;
    }
    exe_path[len] = '\0';
    
    char command[MAX_PATH * 2];
    snprintf(command, sizeof(command), "cp \"%s\" \"%s\"", exe_path, INSTALL_PATH);
    
    if (system(command) == 0 && chmod(INSTALL_PATH, 0755) == 0) {
        printf("%s[SUCCESS]%s Installed to %s\n", GREEN, RESET, INSTALL_PATH);
        
        printf("\n%sWould you like to::%s\n", YELLOW, RESET);
        printf("1. Enable performance mode on boot? (y/N): ");
        
        char response[10];
        if (fgets(response, sizeof(response), stdin) && 
            (response[0] == 'y' || response[0] == 'Y')) {
            create_systemd_service();
        }
        
        printf("\nYou can now use: cpu-governor performance\n");
        return 1;
    } else {
        print_colored(RED, "ERROR", "Installation failed");
        return 0;
    }
}

#define ARXOS_VERSION "0.0.1"   /* ARXOS: every tool pinned to 0.0.1 */
void show_usage() {
    printf("%sCPU Governor - Maximum Performance Controller%s  %sv" ARXOS_VERSION " (ARXOS)%s\n\n", CYAN, RESET, YELLOW, RESET);
    
    printf("%sUsage:%s\n", YELLOW, RESET);
    printf("  %s <command>      - Execute command\n\n", "cpu-governor");
    
    printf("%sCommands:%s\n", YELLOW, RESET);
    printf("  %-15s - Maximum performance mode (recommended)\n", "performance");
    printf("  %-15s - Power saving mode\n", "powersave");
    printf("  %-15s - Show current CPU status\n", "status");
    printf("  %-15s - Install system-wide (requires sudo)\n", "install");
    printf("  %-15s - Show this help\n\n", "help");
    
    printf("%sGovernors:%s\n", YELLOW, RESET);
    for (int i = 0; i < num_governors; i++) {
        printf("  %-12s - %s\n", governors[i].name, governors[i].description);
    }
    
    printf("\n%sQuick Start:%s\n", YELLOW, RESET);
    printf("  sudo cpu-governor performance  # Maximum performance\n");
    printf("  cpu-governor status            # Check current settings\n");
    
    printf("\n%sFirst Time Setup:%s\n", YELLOW, RESET);
    printf("  sudo ./cpu-governor install    # Install system-wide\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        show_usage();
        return 1;
    }
    
    const char* command = argv[1];
    
    if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        show_usage();
        return 0;
    }

    if (strcmp(command, "version") == 0 || strcmp(command, "-V") == 0 || strcmp(command, "--version") == 0) {
        printf("cpu-governor %s (ARXOS)\n", ARXOS_VERSION);
        return 0;
    }
    
    if (strcmp(command, "status") == 0) {
        show_current_status();
        return 0;
    }
    
    if (strcmp(command, "install") == 0) {
        if (!is_root()) {
            print_colored(RED, "ERROR", "Installation requires root privileges (use sudo)");
            return 1;
        }
        return install_systemwide() ? 0 : 1;
    }
    
    if (!is_root()) {
        print_colored(RED, "ERROR", "Setting governors requires root privileges (use sudo)");
        printf("Use 'cpu-governor status' to check current settings\n");
        return 1;
    }
    
    if (strcmp(command, "performance") == 0) {
        performance_mode();
    } else if (strcmp(command, "powersave") == 0) {
        powersave_mode();
    } else {
        if (!set_governor(command)) {
            printf("Use 'cpu-governor help' for usage information\n");
            return 1;
        }
    }
    
    return 0;
}
