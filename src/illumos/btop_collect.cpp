#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <kstat.h>
#include <procfs.h>
#include <pwd.h>
#include <sys/mnttab.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <unistd.h>

#include "../btop_config.hpp"
#include "../btop_log.hpp"
#include "../btop_shared.hpp"
#include "../btop_tools.hpp"

extern "C" int getloadavg(double[], int);

using std::string;
using std::tuple;
using std::vector;

namespace Tools {
	double system_uptime() {
		kstat_ctl_t* kc = kstat_open();
		if (kc == nullptr)
			return 0.0;

		double uptime = 0.0;

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("unix"),
			0,
			const_cast<char*>("system_misc")
		);

		if (ksp != nullptr && kstat_read(kc, ksp, nullptr) != -1) {
			auto* boot_time = static_cast<kstat_named_t*>(
				kstat_data_lookup(
					ksp,
					const_cast<char*>("boot_time")
				)
			);

			if (boot_time != nullptr) {
				const time_t now = time(nullptr);
				const time_t boot =
					static_cast<time_t>(boot_time->value.ui32);

				uptime = difftime(now, boot);
			}
		}

		kstat_close(kc);
		return uptime;
	}
}

namespace Cpu {
	string get_cpuName();
}

namespace Shared {
	long coreCount = 1;
	long page_size = 4096;
	long clk_tck = 100;

	void init() {
		coreCount = sysconf(_SC_NPROCESSORS_ONLN);
		page_size = sysconf(_SC_PAGESIZE);
		clk_tck = sysconf(_SC_CLK_TCK);

		Cpu::cpuName = Cpu::get_cpuName();
		Cpu::cpuHz = Cpu::get_cpuHz();

		if (coreCount < 1)
			coreCount = 1;
		if (page_size < 1)
			page_size = 4096;
		if (clk_tck < 1)
			clk_tck = 100;
	}
}

namespace Cpu {
	vector<uint64_t> core_old_totals;
	vector<uint64_t> core_old_users;
	vector<uint64_t> core_old_kernels;
	vector<uint64_t> core_old_idles;
	vector<uint64_t> core_old_waits;

	vector<string> available_fields = {
		"Auto",
		"total",
		"user",
		"system",
		"idle",
		"iowait"
	};

	vector<string> available_sensors = {"Auto"};
	cpu_info current_cpu;

	bool got_sensors = false;
	bool cpu_temp_only = false;
	bool supports_watts = false;
	bool has_battery = false;

	string cpuName = "Unknown";
	string cpuHz;

	tuple<int, float, long, string> current_bat = {0, 0.0F, 0, ""};
	std::unordered_map<int, int> core_mapping;

	uint64_t get_kstat_uint64(kstat_t* ksp, const char* name) {
		auto* value = static_cast<kstat_named_t*>(
			kstat_data_lookup(ksp, const_cast<char*>(name))
		);

		if (value == nullptr)
			return 0;

		switch (value->data_type) {
			case KSTAT_DATA_UINT64:
				return value->value.ui64;
			case KSTAT_DATA_INT64:
				return static_cast<uint64_t>(
					std::max<int64_t>(0, value->value.i64)
				);
			case KSTAT_DATA_UINT32:
				return value->value.ui32;
			case KSTAT_DATA_INT32:
				return static_cast<uint64_t>(
					std::max<int32_t>(0, value->value.i32)
				);
			default:
				return 0;
		}
	}

	auto get_core_mapping() -> std::unordered_map<int, int> {
		return {};
	}

	string get_cpuName() {
		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return "Unknown";

		string name = "Unknown";

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("cpu_info"),
			0,
			const_cast<char*>("cpu_info0")
		);

		if (ksp != nullptr && kstat_read(kc, ksp, nullptr) != -1) {
			auto* brand = static_cast<kstat_named_t*>(
				kstat_data_lookup(
					ksp,
					const_cast<char*>("brand")
				)
			);

			if (brand != nullptr &&
				brand->data_type == KSTAT_DATA_STRING) {
				const char* value = KSTAT_NAMED_STR_PTR(brand);

				if (value != nullptr && value[0] != '\0')
					name = value;
			}
		}

		kstat_close(kc);

		if (name != "Unknown")
			name = trim_name(name);

		return name;
	}

	auto get_cpuHz() -> string {
		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return {};

		string frequency;

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("cpu_info"),
			0,
			const_cast<char*>("cpu_info0")
		);

		if (ksp != nullptr &&
			kstat_read(kc, ksp, nullptr) != -1) {
			auto* clock_hz = static_cast<kstat_named_t*>(
				kstat_data_lookup(
					ksp,
					const_cast<char*>("current_clock_Hz")
				)
			);

			uint64_t hz = 0;

			if (clock_hz != nullptr) {
				switch (clock_hz->data_type) {
					case KSTAT_DATA_UINT64:
						hz = clock_hz->value.ui64;
						break;

					case KSTAT_DATA_INT64:
						if (clock_hz->value.i64 > 0)
							hz = static_cast<uint64_t>(
								clock_hz->value.i64
							);
						break;

					case KSTAT_DATA_UINT32:
						hz = clock_hz->value.ui32;
						break;

					case KSTAT_DATA_INT32:
						if (clock_hz->value.i32 > 0)
							hz = static_cast<uint64_t>(
								clock_hz->value.i32
							);
						break;

					default:
						break;
				}
			}

			if (hz > 0) {
				const double ghz =
					static_cast<double>(hz) / 1'000'000'000.0;

				frequency = fmt::format("{:.1f} GHz", ghz);
			}
		}

		kstat_close(kc);
		return frequency;
	}

	auto get_battery() -> tuple<int, float, long, string> {
		return {0, 0.0F, 0, ""};
	}

	auto collect(bool no_update) -> cpu_info& {
		if (current_cpu.core_percent.size() !=
			static_cast<size_t>(Shared::coreCount)) {
			current_cpu.core_percent.clear();
			current_cpu.core_percent.resize(Shared::coreCount);

			core_old_totals.assign(Shared::coreCount, 0);
			core_old_users.assign(Shared::coreCount, 0);
			core_old_kernels.assign(Shared::coreCount, 0);
			core_old_idles.assign(Shared::coreCount, 0);
			core_old_waits.assign(Shared::coreCount, 0);
		}

		double load_average[3] = {0.0, 0.0, 0.0};

		if (getloadavg(load_average, 3) == 3) {
			current_cpu.load_avg = {
				load_average[0],
				load_average[1],
				load_average[2]
			};
		}
		else {
			current_cpu.load_avg = {0.0, 0.0, 0.0};
		}

		if (no_update)
			return current_cpu;

		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return current_cpu;

		uint64_t aggregate_user = 0;
		uint64_t aggregate_kernel = 0;
		uint64_t aggregate_idle = 0;
		uint64_t aggregate_wait = 0;
		uint64_t aggregate_total = 0;

		for (long cpu = 0; cpu < Shared::coreCount; ++cpu) {
			kstat_t* ksp = kstat_lookup(
				kc,
				const_cast<char*>("cpu"),
				static_cast<int>(cpu),
				const_cast<char*>("sys")
			);

			if (ksp == nullptr ||
				kstat_read(kc, ksp, nullptr) == -1) {
				current_cpu.core_percent[cpu].push_back(0);

				while (std::cmp_greater(
						current_cpu.core_percent[cpu].size(),
						width * 2
						)) {
					current_cpu.core_percent[cpu].pop_front();
				}

				continue;
			}

			const uint64_t user =
				get_kstat_uint64(ksp, "cpu_ticks_user");
			const uint64_t kernel =
				get_kstat_uint64(ksp, "cpu_ticks_kernel");
			const uint64_t idle =
				get_kstat_uint64(ksp, "cpu_ticks_idle");
			const uint64_t wait =
				get_kstat_uint64(ksp, "cpu_ticks_wait");

			const uint64_t total = user + kernel + idle + wait;

			uint64_t delta_user = 0;
			uint64_t delta_kernel = 0;
			uint64_t delta_idle = 0;
			uint64_t delta_wait = 0;
			uint64_t delta_total = 0;

			if (core_old_totals[cpu] != 0 &&
				total >= core_old_totals[cpu]) {
				delta_user = user - core_old_users[cpu];
				delta_kernel = kernel - core_old_kernels[cpu];
				delta_idle = idle - core_old_idles[cpu];
				delta_wait = wait - core_old_waits[cpu];
				delta_total = total - core_old_totals[cpu];
			}

			core_old_users[cpu] = user;
			core_old_kernels[cpu] = kernel;
			core_old_idles[cpu] = idle;
			core_old_waits[cpu] = wait;
			core_old_totals[cpu] = total;

			long long core_usage = 0;

			if (delta_total > 0) {
				const uint64_t busy =
					delta_total - delta_idle;

				core_usage = static_cast<long long>(
					(busy * 100ULL) / delta_total
				);
			}

			current_cpu.core_percent[cpu].push_back(core_usage);

			while (std::cmp_greater(
					current_cpu.core_percent[cpu].size(),
					width * 2
					)) {
				current_cpu.core_percent[cpu].pop_front();
			}

			aggregate_user += delta_user;
			aggregate_kernel += delta_kernel;
			aggregate_idle += delta_idle;
			aggregate_wait += delta_wait;
			aggregate_total += delta_total;
		}

		kstat_close(kc);

		long long user_percent = 0;
		long long kernel_percent = 0;
		long long idle_percent = 100;
		long long wait_percent = 0;
		long long total_percent = 0;

		if (aggregate_total > 0) {
			user_percent = static_cast<long long>(
				aggregate_user * 100ULL / aggregate_total
			);

			kernel_percent = static_cast<long long>(
				aggregate_kernel * 100ULL / aggregate_total
			);

			idle_percent = static_cast<long long>(
				aggregate_idle * 100ULL / aggregate_total
			);

			wait_percent = static_cast<long long>(
				aggregate_wait * 100ULL / aggregate_total
			);

			total_percent = std::clamp<long long>(
				100 - idle_percent,
				0,
				100
			);
		}

		for (auto& [name, values] : current_cpu.cpu_percent) {
			if (name == "total")
				values.push_back(total_percent);
			else if (name == "user")
				values.push_back(user_percent);
			else if (name == "system")
				values.push_back(kernel_percent);
			else if (name == "idle")
				values.push_back(idle_percent);
			else if (name == "iowait")
				values.push_back(wait_percent);
			else
				values.push_back(0);

			while (std::cmp_greater(
					values.size(),
					width * 2
					)) {
				values.pop_front();
			}
		}

		return current_cpu;
	}
}

namespace Mem {
	bool has_swap = false;
	int disk_ios = 0;
	mem_info current_mem;
	vector<string> last_found;

	uint64_t get_page_count(kstat_t* ksp, const char* name) {
		auto* value = static_cast<kstat_named_t*>(
			kstat_data_lookup(
				ksp,
				const_cast<char*>(name)
			)
		);

		if (value == nullptr)
			return 0;

		switch (value->data_type) {
			case KSTAT_DATA_UINT64:
				return value->value.ui64;

			case KSTAT_DATA_INT64:
				return value->value.i64 > 0
					? static_cast<uint64_t>(value->value.i64)
					: 0;

			case KSTAT_DATA_UINT32:
				return value->value.ui32;

			case KSTAT_DATA_INT32:
				return value->value.i32 > 0
					? static_cast<uint64_t>(value->value.i32)
					: 0;

			default:
				return 0;
		}
	}

	bool is_pseudo_filesystem(const string& fstype) {
		static const std::unordered_set<string> excluded = {
			"autofs",
			"bootfs",
			"ctfs",
			"dev",
			"devfs",
			"fd",
			"lofs",
			"mntfs",
			"objfs",
			"proc",
			"sharefs",
			"tmpfs"
		};

		return excluded.contains(fstype);
	}

	void collect_zpool_io() {
		disk_ios = 0;

		auto& disks = current_mem.disks;

		if (!disks.contains("/"))
			return;

		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return;

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("zfs"),
			0,
			const_cast<char*>("rpool")
		);

		if (ksp == nullptr ||
			kstat_read(kc, ksp, nullptr) == -1) {
			kstat_close(kc);
			return;
		}

		if (ksp->ks_type != KSTAT_TYPE_IO ||
			ksp->ks_data == nullptr) {
			kstat_close(kc);
			return;
		}

		const auto* io =
			static_cast<const kstat_io_t*>(ksp->ks_data);

		const uint64_t nread =
			static_cast<uint64_t>(io->nread);

		const uint64_t nwritten =
			static_cast<uint64_t>(io->nwritten);

		const uint64_t snapshot_time =
			static_cast<uint64_t>(ksp->ks_snaptime);

		const uint64_t cumulative_busy =
			static_cast<uint64_t>(io->rtime) +
			static_cast<uint64_t>(io->wtime);

		auto& disk = disks.at("/");

		long long read_delta = 0;
		long long write_delta = 0;
		long long activity = 0;

		if (disk.old_io[2] > 0 &&
			snapshot_time >
			static_cast<uint64_t>(disk.old_io[2])) {
			if (nread >=
				static_cast<uint64_t>(disk.old_io[0])) {
				read_delta =
					static_cast<long long>(
						nread -
						static_cast<uint64_t>(
							disk.old_io[0]
						)
					);
			}

			if (nwritten >=
				static_cast<uint64_t>(disk.old_io[1])) {
				write_delta =
					static_cast<long long>(
						nwritten -
						static_cast<uint64_t>(
							disk.old_io[1]
						)
					);
			}

			static uint64_t old_busy = 0;

			if (old_busy > 0 &&
				cumulative_busy >= old_busy) {
				const uint64_t elapsed =
					snapshot_time -
					static_cast<uint64_t>(
						disk.old_io[2]
					);

				const uint64_t busy_delta =
					cumulative_busy - old_busy;

				if (elapsed > 0) {
					activity =
						std::clamp<long long>(
							static_cast<long long>(
								busy_delta *
								100ULL /
								elapsed
							),
							0,
							100
						);
				}
			}

			old_busy = cumulative_busy;
		}

		disk.old_io[0] =
			static_cast<int64_t>(nread);

		disk.old_io[1] =
			static_cast<int64_t>(nwritten);

		disk.old_io[2] =
			static_cast<int64_t>(snapshot_time);

		disk.io_read.push_back(read_delta);
		disk.io_write.push_back(write_delta);
		disk.io_activity.push_back(activity);

		while (std::cmp_greater(
				disk.io_read.size(),
				width * 2
				)) {
			disk.io_read.pop_front();
		}

		while (std::cmp_greater(
				disk.io_write.size(),
				width * 2
				)) {
			disk.io_write.pop_front();
		}

		while (std::cmp_greater(
				disk.io_activity.size(),
				width * 2
				)) {
			disk.io_activity.pop_front();
		}

		disk_ios = 1;

		kstat_close(kc);
	}

	void collect_swap() {
		auto& stats = current_mem.stats;

		stats["swap_total"] = 0;
		stats["swap_used"] = 0;
		stats["swap_free"] = 0;
		has_swap = false;

		if (!Config::getB("show_swap"))
			return;

		const int count = swapctl(SC_GETNSWP, nullptr);

		if (count <= 0)
			return;

		const size_t table_size =
			sizeof(swaptbl_t) +
			static_cast<size_t>(count - 1) *
			sizeof(swapent_t);

		vector<char> table_storage(table_size);
		vector<char> path_storage(
			static_cast<size_t>(count) * MAXPATHLEN
		);

		auto* table =
			reinterpret_cast<swaptbl_t*>(
				table_storage.data()
			);

		table->swt_n = count;

		for (int index = 0; index < count; ++index) {
			table->swt_ent[index].ste_path =
				path_storage.data() +
				static_cast<size_t>(index) *
				MAXPATHLEN;
		}

		const int listed = swapctl(SC_LIST, table);

		if (listed < 0)
			return;

		uint64_t total_pages = 0;
		uint64_t free_pages = 0;

		const int entries = std::min(listed, count);

		for (int index = 0; index < entries; ++index) {
			const auto& entry = table->swt_ent[index];

			if ((entry.ste_flags &
				(ST_INDEL | ST_DOINGDEL)) != 0) {
				continue;
			}

			if (entry.ste_pages > 0) {
				total_pages +=
					static_cast<uint64_t>(
						entry.ste_pages
					);
			}

			if (entry.ste_free > 0) {
				free_pages +=
					static_cast<uint64_t>(
						entry.ste_free
					);
			}
		}

		if (free_pages > total_pages)
			free_pages = total_pages;

		const uint64_t page_size =
			static_cast<uint64_t>(
				Shared::page_size
			);

		const uint64_t total =
			total_pages * page_size;

		const uint64_t free =
			free_pages * page_size;

		const uint64_t used =
			total >= free
				? total - free
				: 0;

		stats["swap_total"] = total;
		stats["swap_used"] = used;
		stats["swap_free"] = free;

		has_swap = total > 0;
	}

	uint64_t get_totalMem() {
		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return 0;

		uint64_t total = 0;

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("unix"),
			0,
			const_cast<char*>("system_pages")
		);

		if (ksp != nullptr &&
			kstat_read(kc, ksp, nullptr) != -1) {
			const uint64_t pages =
				get_page_count(ksp, "physmem");

			total = pages *
				static_cast<uint64_t>(Shared::page_size);
		}

		kstat_close(kc);
		return total;
	}

	auto collect(bool no_update) -> mem_info& {
		if (no_update)
			return current_mem;

		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return current_mem;

		uint64_t total_pages = 0;
		uint64_t free_pages = 0;
		uint64_t available_pages = 0;

		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("unix"),
			0,
			const_cast<char*>("system_pages")
		);

		if (ksp != nullptr &&
			kstat_read(kc, ksp, nullptr) != -1) {
			total_pages =
				get_page_count(ksp, "physmem");

			free_pages =
				get_page_count(ksp, "freemem");

			available_pages =
				get_page_count(ksp, "availrmem");
		}

		kstat_close(kc);

		const uint64_t page_size =
			static_cast<uint64_t>(Shared::page_size);

		const uint64_t total =
			total_pages * page_size;

		const uint64_t free =
			free_pages * page_size;

		const uint64_t available =
			available_pages * page_size;

		const uint64_t cached =
			available > free
				? available - free
				: 0;

		const uint64_t used =
			total > available
				? total - available
				: 0;

		current_mem.stats["used"] = used;
		current_mem.stats["available"] = available;
		current_mem.stats["cached"] = cached;
		current_mem.stats["free"] = free;

		collect_swap();

		for (auto& [name, values] : current_mem.percent) {
			uint64_t value = 0;
			uint64_t divisor = total;

			if (name == "used")
				value = used;
			else if (name == "available")
				value = available;
			else if (name == "cached")
				value = cached;
			else if (name == "free")
				value = free;
			else if (name == "swap_used") {
				value =
					current_mem.stats["swap_used"];

				divisor =
					current_mem.stats["swap_total"];
			}
			else if (name == "swap_free") {
				value =
					current_mem.stats["swap_free"];

				divisor =
					current_mem.stats["swap_total"];
			}
			else {
				value = 0;
				divisor = 0;
			}

			const long long percentage =
				divisor > 0
					? static_cast<long long>(
						value * 100ULL / divisor
					)
					: 0;

			values.push_back(percentage);

			while (std::cmp_greater(
					values.size(),
					width * 2
					)) {
				values.pop_front();
			}
		}

		if (Config::getB("show_disks")) {
			auto& disks = current_mem.disks;
			const string& disks_filter =
				Config::getS("disks_filter");

			vector<string> filter;
			bool filter_exclude = false;

			if (!disks_filter.empty()) {
				std::istringstream filter_stream(
					disks_filter
				);

				string item;

				while (filter_stream >> item)
					filter.push_back(item);

				if (!filter.empty() &&
					filter.front().starts_with(
						"exclude="
					)) {
					filter_exclude = true;

					filter.front() =
						filter.front().substr(8);

					if (filter.front().empty())
						filter.erase(filter.begin());
				}
			}

			FILE* mount_file = fopen(MNTTAB, "r");
			vector<string> found;

			if (mount_file != nullptr) {
				struct mnttab mount_entry {};

				while (getmntent(
						mount_file,
						&mount_entry
						) == 0) {
					if (mount_entry.mnt_mountp ==
						nullptr ||
						mount_entry.mnt_special ==
						nullptr ||
						mount_entry.mnt_fstype ==
						nullptr) {
						continue;
					}

					const string mountpoint =
						mount_entry.mnt_mountp;

					const string device =
						mount_entry.mnt_special;

					const string fstype =
						mount_entry.mnt_fstype;

					if (is_pseudo_filesystem(fstype))
						continue;

					if (!filter.empty()) {
						const bool match =
							std::ranges::find(
								filter,
								mountpoint
							) != filter.end();

						if ((filter_exclude &&
							match) ||
							(!filter_exclude &&
							!match)) {
							continue;
						}
					}

					struct statvfs vfs {};

					if (statvfs(
							mountpoint.c_str(),
							&vfs
						) != 0) {
						Logger::warning(
							"Failed to get "
							"filesystem stats "
							"for: {}",
							mountpoint
						);

						continue;
					}

					found.push_back(mountpoint);

					if (!disks.contains(mountpoint)) {
						disk_info disk;

						disk.dev = device;
						disk.name =
							mountpoint == "/"
								? "root"
								: mountpoint;

						disk.fstype = fstype;

						disks.emplace(
							mountpoint,
							std::move(disk)
						);

						redraw = true;
					}

					auto& disk =
						disks.at(mountpoint);

					disk.dev = device;
					disk.fstype = fstype;

					const uint64_t fragment_size =
						vfs.f_frsize > 0
							? vfs.f_frsize
							: vfs.f_bsize;

					disk.total =
						static_cast<int64_t>(
							vfs.f_blocks
						) * fragment_size;

					disk.free =
						static_cast<int64_t>(
							vfs.f_bfree
						) * fragment_size;

					disk.used =
						disk.total > disk.free
							? disk.total -
							  disk.free
							: 0;

					if (disk.total > 0) {
						disk.used_percent =
							static_cast<int>(
								std::round(
									static_cast<double>(
										disk.used
									) *
									100.0 /
									static_cast<double>(
										disk.total
									)
								)
							);

						disk.free_percent =
							100 -
							disk.used_percent;
					}
					else {
						disk.used_percent = 0;
						disk.free_percent = 0;
					}
				}

				fclose(mount_file);
			}

			const bool swap_as_disk =
				Config::getB("swap_disk") &&
				has_swap;

			if (swap_as_disk)
				found.push_back("swap");

			for (auto iterator = disks.begin();
				iterator != disks.end();) {
				if (std::ranges::find(
						found,
						iterator->first
					) == found.end()) {
					iterator = disks.erase(iterator);
				}
				else {
					++iterator;
				}
			}

			if (found.size() != last_found.size())
				redraw = true;

			last_found = found;

			current_mem.disks_order.clear();

			if (disks.contains("/"))
				current_mem.disks_order.push_back("/");

			if (swap_as_disk) {
				if (!disks.contains("swap")) {
					disk_info swap_entry;
					swap_entry.name = "swap";

					disks.emplace(
						"swap",
						std::move(swap_entry)
					);
				}

				auto& swap = disks.at("swap");

				swap.total =
					current_mem.stats["swap_total"];

				swap.used =
					current_mem.stats["swap_used"];

				swap.free =
					current_mem.stats["swap_free"];

				swap.used_percent =
					current_mem.percent[
						"swap_used"
					].back();

				swap.free_percent =
					current_mem.percent[
						"swap_free"
					].back();

				current_mem.disks_order.push_back(
					"swap"
				);
			}

			for (const auto& mountpoint : found) {
				if (mountpoint != "/" &&
					mountpoint != "swap" &&
					disks.contains(mountpoint)) {
					current_mem.disks_order.push_back(
						mountpoint
					);
				}
			}

			collect_zpool_io();
		}

		return current_mem;
	}

}

namespace Net {
	vector<string> interfaces;
	string selected_iface;
	bool rescale = true;

	std::unordered_map<string, uint64_t> graph_max = {
		{"download", 0},
		{"upload", 0}
	};

	std::unordered_map<string, std::array<int, 2>> max_count = {
		{"download", {0, 0}},
		{"upload", {0, 0}}
	};

	std::unordered_map<string, net_info> current_net;

	std::unordered_map<string, uint64_t> old_download;
	std::unordered_map<string, uint64_t> old_upload;
	std::unordered_map<string, std::chrono::steady_clock::time_point> old_time;

	uint64_t get_link_counter(
		kstat_ctl_t* kc,
		const string& interface,
		const char* counter
	) {
		kstat_t* ksp = kstat_lookup(
			kc,
			const_cast<char*>("link"),
			0,
			const_cast<char*>(interface.c_str())
		);

		if (ksp == nullptr ||
			kstat_read(kc, ksp, nullptr) == -1) {
			return 0;
		}

		auto* value = static_cast<kstat_named_t*>(
			kstat_data_lookup(
				ksp,
				const_cast<char*>(counter)
			)
		);

		if (value == nullptr)
			return 0;

		switch (value->data_type) {
			case KSTAT_DATA_UINT64:
				return value->value.ui64;

			case KSTAT_DATA_INT64:
				return value->value.i64 > 0
					? static_cast<uint64_t>(
						value->value.i64
					)
					: 0;

			case KSTAT_DATA_UINT32:
				return value->value.ui32;

			case KSTAT_DATA_INT32:
				return value->value.i32 > 0
					? static_cast<uint64_t>(
						value->value.i32
					)
					: 0;

			default:
				return 0;
		}
	}

	auto collect(bool no_update) -> net_info& {
		static net_info empty;

		const bool net_auto =
				Config::getB("net_auto");

		const bool net_sync =
				Config::getB("net_sync");

		if (no_update && !selected_iface.empty())
			return current_net[selected_iface];

		interfaces.clear();

		IfAddrsPtr addresses;

		if (addresses.get_status() != 0)
			return empty;

		for (ifaddrs* current = addresses.get();
			current != nullptr;
			current = current->ifa_next) {
			if (current->ifa_name == nullptr ||
				current->ifa_addr == nullptr) {
				continue;
			}

			const string interface = current->ifa_name;

			if ((current->ifa_flags & IFF_LOOPBACK) != 0)
				continue;

			if ((current->ifa_flags & IFF_UP) == 0)
				continue;

			if (std::ranges::find(
					interfaces,
					interface
				) == interfaces.end()) {
				interfaces.push_back(interface);
			}

			auto& info = current_net[interface];

			char address_buffer[INET6_ADDRSTRLEN] = {};

			if (current->ifa_addr->sa_family == AF_INET) {
				const auto* address =
					reinterpret_cast<sockaddr_in*>(
						current->ifa_addr
					);

				if (inet_ntop(
						AF_INET,
						&address->sin_addr,
						address_buffer,
						sizeof(address_buffer)
					) != nullptr) {
					info.ipv4 = address_buffer;
				}
			}
			else if (current->ifa_addr->sa_family == AF_INET6) {
				const auto* address =
					reinterpret_cast<sockaddr_in6*>(
						current->ifa_addr
					);

				if (inet_ntop(
						AF_INET6,
						&address->sin6_addr,
						address_buffer,
						sizeof(address_buffer)
					) != nullptr) {
					info.ipv6 = address_buffer;
				}
			}

			info.connected = true;
		}

		if (interfaces.empty()) {
			selected_iface.clear();
			return empty;
		}

		std::ranges::sort(interfaces);

		if (selected_iface.empty() ||
			std::ranges::find(
				interfaces,
				selected_iface
			) == interfaces.end()) {
			selected_iface = interfaces.front();
		}

		kstat_ctl_t* kc = kstat_open();

		if (kc == nullptr)
			return current_net[selected_iface];

		const auto now =
			std::chrono::steady_clock::now();

		for (const auto& interface : interfaces) {
			const uint64_t received =
				get_link_counter(
					kc,
					interface,
					"rbytes64"
				);

			const uint64_t transmitted =
				get_link_counter(
					kc,
					interface,
					"obytes64"
				);

			auto& info = current_net[interface];

			uint64_t download_speed = 0;
			uint64_t upload_speed = 0;

			if (old_time.contains(interface)) {
				const double elapsed =
					std::chrono::duration<double>(
						now - old_time[interface]
					).count();

				if (elapsed > 0.0) {
					if (received >=
						old_download[interface]) {
						download_speed =
							static_cast<uint64_t>(
								(
									received -
									old_download[
									interface
									]
								) / elapsed
							);
					}

					if (transmitted >=
						old_upload[interface]) {
						upload_speed =
							static_cast<uint64_t>(
								(
									transmitted -
									old_upload[
									interface
									]
								) / elapsed
							);
					}
				}
			}

			old_download[interface] = received;
			old_upload[interface] = transmitted;
			old_time[interface] = now;

			auto& download =
				info.stat["download"];

			auto& upload =
				info.stat["upload"];

			download.speed = download_speed;
			download.total = received;
			download.last = received;

			upload.speed = upload_speed;
			upload.total = transmitted;
			upload.last = transmitted;

			info.bandwidth["download"].push_back(
				static_cast<long long>(
					download_speed
				)
			);

			info.bandwidth["upload"].push_back(
				static_cast<long long>(
					upload_speed
				)
			);

			while (std::cmp_greater(
					info.bandwidth["download"].size(),
					width * 2
					)) {
				info.bandwidth["download"].pop_front();
			}

			while (std::cmp_greater(
					info.bandwidth["upload"].size(),
					width * 2
					)) {
				info.bandwidth["upload"].pop_front();
			}

			download.top = std::max(
				download.top,
				download_speed
			);

			upload.top = std::max(
				upload.top,
				upload_speed
			);
		}

		kstat_close(kc);

		if (net_auto) {
			bool sync = false;

			for (const string direction :
				{"download", "upload"}) {
				const auto& bandwidth =
					current_net[selected_iface]
						.bandwidth[direction];

				const uint64_t speed =
					current_net[selected_iface]
						.stat[direction]
						.speed;

				if (speed > graph_max[direction]) {
					max_count[direction][0]++;

					if (max_count[direction][1] > 0)
						max_count[direction][1]--;
				}
				else if (
					graph_max[direction] >
						(10ULL << 10) &&
					speed <
						graph_max[direction] / 10
				) {
					max_count[direction][1]++;

					if (max_count[direction][0] > 0)
						max_count[direction][0]--;
				}

				for (const int selector : {0, 1}) {
					if (
						rescale ||
						max_count[direction][selector] >= 5
					) {
						long long average_speed =
							static_cast<long long>(
								speed
							);

						if (bandwidth.size() >= 5) {
							average_speed =
								std::accumulate(
									bandwidth.rbegin(),
									bandwidth.rbegin() + 5,
									0LL
								) / 5;
						}

						graph_max[direction] =
							std::max<uint64_t>(
								static_cast<uint64_t>(
									average_speed *
									(
										selector == 0
										? 1.3
										: 3.0
									)
								),
								10ULL << 10
							);

						max_count[direction] = {0, 0};
						redraw = true;

						if (net_sync)
							sync = true;

						break;
					}
				}

				if (sync) {
					const string other =
						direction == "upload"
						? "download"
						: "upload";

					graph_max[other] =
						graph_max[direction];

					max_count[other] = {0, 0};
					break;
				}
			}
		}

		rescale = false;

		return current_net[selected_iface];
	}
}

namespace Proc {
	atomic<int> numpids = 0;

	int collapse = -1;
	int expand = -1;
	int filter_found = 0;
	int toggle_children = -1;
	int collapse_all = -1;

	vector<proc_info> current_procs;

	detail_container detailed;

	string get_status(char status) {
		const auto it = proc_states.find(status);
		return it != proc_states.end() ? it->second : "Unknown";
	}

	//* Get detailed info for selected process
	void collect_details(
		const size_t pid,
		vector<proc_info>& processes
	) {
		if (pid != detailed.last_pid) {
			detailed = {};
			detailed.last_pid = pid;
			detailed.skip_smaps =
				!Config::getB("proc_info_smaps");
		}

		const auto process = std::ranges::find(
			processes,
			pid,
			&proc_info::pid
		);

		if (process == processes.end()) {
			if (detailed.status != "Dead") {
				detailed.status = "Dead";
				redraw = true;
			}

			return;
		}

		detailed.entry = *process;

		if (!Config::getB("proc_per_core"))
			detailed.entry.cpu_p *= Shared::coreCount;

		detailed.cpu_percent.push_back(
			std::clamp<long long>(
				static_cast<long long>(
					std::round(detailed.entry.cpu_p)
				),
				0,
				100
			)
		);

		while (std::cmp_greater(
				detailed.cpu_percent.size(),
				width
				)) {
			detailed.cpu_percent.pop_front();
		}

		const time_t now = time(nullptr);

		if (detailed.entry.state != 'X' &&
			detailed.entry.cpu_s > 0 &&
			now >= static_cast<time_t>(
				detailed.entry.cpu_s
			)) {
			detailed.elapsed = Tools::sec_to_dhms(
				static_cast<uint64_t>(
					now -
					static_cast<time_t>(
						detailed.entry.cpu_s
					)
				)
			);
		}
		else {
			detailed.elapsed = Tools::sec_to_dhms(
				detailed.entry.death_time
			);
		}

		if (detailed.elapsed.size() > 8)
			detailed.elapsed.resize(
				detailed.elapsed.size() - 3
			);

		detailed.parent.clear();

		const auto parent = std::ranges::find(
			processes,
			detailed.entry.ppid,
			&proc_info::pid
		);

		if (parent != processes.end()) {
			detailed.parent = parent->name;
		}
		else if (detailed.entry.ppid > 0) {
			detailed.parent =
				std::to_string(detailed.entry.ppid);
		}

		detailed.status =
			get_status(detailed.entry.state);

		detailed.mem_bytes.push_back(
			static_cast<long long>(
				detailed.entry.mem
			)
		);

		detailed.memory =
			Tools::floating_humanizer(detailed.entry.mem);

		if (detailed.first_mem == -1 ||
			detailed.first_mem <
				detailed.mem_bytes.back() / 2 ||
			detailed.first_mem >
				detailed.mem_bytes.back() * 4) {
			detailed.first_mem = static_cast<long long>(
				std::min<uint64_t>(
					static_cast<uint64_t>(
						detailed.mem_bytes.back()
					) * 2,
					Mem::get_totalMem()
				)
			);

			redraw = true;
		}

		while (std::cmp_greater(
				detailed.mem_bytes.size(),
				width
				)) {
			detailed.mem_bytes.pop_front();
		}

		detailed.io_read.clear();
		detailed.io_write.clear();
	}

	auto collect(bool no_update) -> vector<proc_info>& {
		const string& sorting = Config::getS("proc_sorting");
		const bool reverse = Config::getB("proc_reversed");
		const string& filter = Config::getS("proc_filter");
		const bool per_core = Config::getB("proc_per_core");
		const bool tree = Config::getB("proc_tree");
		const bool pause_proc_list = Config::getB("pause_proc_list");
		const bool show_detailed = Config::getB("show_detailed");
		const size_t detailed_pid = Config::getI("detailed_pid");

		static string current_sort;
		static string current_filter;
		static bool current_reverse = false;
		static bool current_tree = false;

		const bool filter_changed = current_filter != filter;
		const bool sort_changed =
			current_sort != sorting ||
			current_reverse != reverse;

		const bool tree_changed =
			current_tree != tree;

		if (filter_changed)
			current_filter = filter;

		if (sort_changed) {
			current_sort = sorting;
			current_reverse = reverse;
		}

		if (tree_changed)
			current_tree = tree;

		if (!no_update && !pause_proc_list) {
			vector<proc_info> new_processes;

			std::error_code error;

			for (const auto& entry :
				std::filesystem::directory_iterator("/proc", error)) {
				if (error)
					break;

				if (!entry.is_directory(error))
					continue;

				const string pid_text =
					entry.path().filename().string();

				if (pid_text.empty() ||
					!std::ranges::all_of(
						pid_text,
						[](unsigned char c) {
							return std::isdigit(c) != 0;
						}
					)) {
					continue;
				}

				const std::filesystem::path psinfo_path =
					entry.path() / "psinfo";

				std::ifstream input(
					psinfo_path,
					std::ios::in | std::ios::binary
				);

				if (!input)
					continue;

				psinfo_t psinfo{};

				input.read(
					reinterpret_cast<char*>(&psinfo),
					sizeof(psinfo)
				);

				if (input.gcount() !=
					static_cast<std::streamsize>(sizeof(psinfo))) {
					continue;
				}

				if (psinfo.pr_pid < 1)
					continue;

				proc_info process;

				process.pid =
					static_cast<size_t>(psinfo.pr_pid);

				const auto previous =
					std::ranges::find(
						current_procs,
						process.pid,
						&proc_info::pid
					);

				if (previous != current_procs.end())
					process.collapsed =
						previous->collapsed;

				process.ppid =
					static_cast<uint64_t>(psinfo.pr_ppid);

				process.name = string(
					psinfo.pr_fname,
					strnlen(psinfo.pr_fname, PRFNSZ)
				);

				process.cmd = string(
					psinfo.pr_psargs,
					strnlen(psinfo.pr_psargs, PRARGSZ)
				);

				if (process.name.empty())
					process.name = std::to_string(process.pid);

				if (process.cmd.empty())
					process.cmd = process.name;

				process.short_cmd = process.name;

				process.threads =
					psinfo.pr_nlwp > 0
						? static_cast<size_t>(
							psinfo.pr_nlwp
						)
						: 0;

				process.mem =
					static_cast<uint64_t>(
						psinfo.pr_rssize
					) * 1024ULL;

				process.p_nice =
					static_cast<int64_t>(
						psinfo.pr_lwp.pr_nice
					);

				process.state = psinfo.pr_lwp.pr_sname;

				/*
				 * illumos may report 'O' for a process currently
				 * executing on a processor. btop uses 'R' for the
				 * corresponding running state.
				 */
				if (process.state == 'O')
					process.state = 'R';

				struct passwd* password =
					getpwuid(psinfo.pr_uid);

				if (password != nullptr &&
					password->pw_name != nullptr) {
					process.user = password->pw_name;
				}
				else {
					process.user =
						std::to_string(psinfo.pr_uid);
				}

				process.cpu_s =
					psinfo.pr_start.tv_sec > 0
						? static_cast<uint64_t>(
							psinfo.pr_start.tv_sec
						)
						: 0;

				const uint64_t cpu_seconds =
					psinfo.pr_time.tv_sec > 0
						? static_cast<uint64_t>(
							psinfo.pr_time.tv_sec
						)
						: 0;

				const uint64_t cpu_nanoseconds =
					psinfo.pr_time.tv_nsec > 0
						? static_cast<uint64_t>(
							psinfo.pr_time.tv_nsec
						)
						: 0;

				process.cpu_t =
					cpu_seconds * 1'000'000ULL +
					cpu_nanoseconds / 1'000ULL;

				/*
				 * pr_pctcpu is a 16-bit fixed-point fraction:
				 * 0x8000 represents 100 percent.
				 */
				double recent_cpu =
					static_cast<double>(
						psinfo.pr_pctcpu
					) * 100.0 / 32768.0;

				if (per_core)
					recent_cpu *= Shared::coreCount;

				process.cpu_p = std::clamp(
					recent_cpu,
					0.0,
					100.0 *
					static_cast<double>(
						Shared::coreCount
					)
				);

				const time_t now = time(nullptr);

				if (process.cpu_s > 0 &&
					now > static_cast<time_t>(
						process.cpu_s
					)) {
					const double elapsed =
						static_cast<double>(
							now - process.cpu_s
						);

					const double consumed =
						static_cast<double>(
							process.cpu_t
						) / 1'000'000.0;

					process.cpu_c = std::clamp(
						consumed * 100.0 / elapsed,
						0.0,
						100.0 *
						static_cast<double>(
							Shared::coreCount
						)
					);
				}
				else {
					process.cpu_c = 0.0;
				}

				new_processes.push_back(
					std::move(process)
				);
			}

			current_procs = std::move(new_processes);
		}

		filter_found = 0;

		for (auto& process : current_procs) {
			if (!filter.empty() &&
				!matches_filter(process, filter)) {
				process.filtered = true;
				filter_found++;
			}
			else {
				process.filtered = false;
			}
		}

		if (!current_procs.empty() &&
			(sort_changed ||
			tree_changed ||
			filter_changed ||
			(!no_update && !pause_proc_list))) {
			proc_sorter(
				current_procs,
				sorting,
				reverse,
				tree
			);
		}

		if (tree && !current_procs.empty()) {
			vector<size_t> found;
			found.reserve(current_procs.size());

			for (const auto& process : current_procs)
				found.push_back(process.pid);

			for (auto& process : current_procs) {
				if (std::ranges::find(
						found,
						process.ppid
					) == found.end()) {
					process.ppid = 0;
				}

				process.depth = 0;
				process.tree_index = 0;
				process.prefix.clear();
			}

			std::ranges::stable_sort(
				current_procs,
				std::ranges::less{},
				&proc_info::ppid
			);

			if (const int find_pid =
					collapse != -1
						? collapse
						: expand;
				find_pid != -1) {
				const auto collapser =
					std::ranges::find(
						current_procs,
						static_cast<size_t>(find_pid),
						&proc_info::pid
					);

				if (collapser != current_procs.end()) {
					if (collapse == expand) {
						collapser->collapsed =
							!collapser->collapsed;
					}
					else if (collapse > -1) {
						collapser->collapsed = true;
					}
					else if (expand > -1) {
						collapser->collapsed = false;
					}
				}

				collapse = -1;
				expand = -1;
			}

			if (collapse_all != -1) {
				toggle_tree_collapse(current_procs);
				collapse_all = -1;
			}

			vector<tree_proc> tree_processes;
			tree_processes.reserve(current_procs.size());

			const auto roots = std::ranges::equal_range(
				current_procs,
				current_procs.front().ppid,
				std::ranges::less{},
				&proc_info::ppid
			);

			for (auto& process : roots) {
				_tree_gen(
					process,
					current_procs,
					tree_processes,
					0,
					false,
					filter,
					false,
					no_update,
					filter_changed
				);
			}

			int tree_index = 0;

			tree_sort(
				tree_processes,
				sorting,
				reverse,
				pause_proc_list &&
					!sort_changed &&
					!tree_changed,
				tree_index,
				static_cast<int>(
					current_procs.size()
				)
			);

			for (auto iterator = tree_processes.begin();
				iterator != tree_processes.end();
				++iterator) {
				_collect_prefixes(
					*iterator,
					iterator ==
						tree_processes.end() - 1
				);
			}

			std::ranges::stable_sort(
				current_procs,
				std::ranges::less{},
				&proc_info::tree_index
			);
		}

		if (show_detailed)
			collect_details(
				detailed_pid,
				current_procs
			);

		numpids = static_cast<int>(
			current_procs.size()
		) - filter_found;

		return current_procs;
	}
}
