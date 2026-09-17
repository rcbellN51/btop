/* Copyright 2026

   Licensed under the Apache License, Version 2.0.
*/

#include <OS.h>
#include <Directory.h>
#include <Entry.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <fs_info.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <pwd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../btop.hpp"
#include "../btop_config.hpp"
#include "../btop_log.hpp"
#include "../btop_shared.hpp"
#include "../btop_tools.hpp"

using std::string;
using std::tuple;
using std::vector;
using namespace Tools;

namespace Cpu {
	vector<bigtime_t> core_old_active;
	bigtime_t old_time = 0;
        std::unordered_map<team_id, bigtime_t> team_old_user;

        vector<string> available_fields = {"Auto", "total", "user", "system"};
	vector<string> available_sensors = {"Auto"};
	cpu_info current_cpu;

	bool got_sensors = false;
	bool cpu_temp_only = false;
	bool supports_watts = false;
	bool has_battery = false;

	string cpuName = "Unknown";
	string cpuHz;

	tuple<int, float, long, string> current_bat = {0, 0, 0, ""};
	std::unordered_map<int, int> core_mapping;

	auto get_core_mapping() -> std::unordered_map<int, int> {
		std::unordered_map<int, int> mapping;

		for (long i = 0; i < Shared::coreCount; i++)
			mapping[i] = i;

		return mapping;
	}

	string get_cpuName() {
		cpuid_info info {};

		if (get_cpuid(&info, 0x80000000, 0) != B_OK or
		    info.regs.eax < 0x80000004)
			return "";

		char brand[49] {};

		for (uint32 leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
			if (get_cpuid(&info, leaf, 0) != B_OK)
				return "";

			char* part = brand + (leaf - 0x80000002) * 16;
			std::memcpy(part, &info.regs.eax, 4);
			std::memcpy(part + 4, &info.regs.ebx, 4);
			std::memcpy(part + 8, &info.regs.ecx, 4);
			std::memcpy(part + 12, &info.regs.edx, 4);
		}

		return trim_name(string(brand));
	}

	static string normalize_frequency(double mhz) {
		string str;
		if (mhz > 999999) {
			str = fmt::format("{:.1f}", mhz / 1'000'000);
			str.resize(3);
			if (str.back() == '.') str.pop_back();
			str += " THz";
		}
		else if (mhz > 999) {
			str = fmt::format("{:.1f}", mhz / 1'000);
			str.resize(3);
			if (str.back() == '.') str.pop_back();
			str += " GHz";
		}
		else {
			str = fmt::format("{:.0f} MHz", mhz);
		}
		return str;
	}

	string get_cpuHz() {
		::cpu_info cpu {};

		if (get_cpu_info(0, 1, &cpu) != B_OK or cpu.current_frequency == 0)
			return "";

		return normalize_frequency(cpu.current_frequency / 1'000'000.0);
	}

	auto get_battery() -> tuple<int, float, long, string> {
		return {0, 0, 0, ""};
	}

	auto collect(bool no_update) -> cpu_info& {
		if (Runner::stopping or
		    (no_update and not current_cpu.cpu_percent.at("total").empty()))
			return current_cpu;

		if (Config::getB("show_cpu_freq")) {
			auto hz = get_cpuHz();
			if (not hz.empty())
				cpuHz = hz;
		}

		system_info system {};
		if (get_system_info(&system) != B_OK) {
			Logger::error("Cpu::collect() -> get_system_info() failed");
			return current_cpu;
		}

		vector<::cpu_info> cpus(system.cpu_count);
		if (get_cpu_info(0, system.cpu_count, cpus.data()) != B_OK) {
			Logger::error("Cpu::collect() -> get_cpu_info() failed");
			return current_cpu;
		}

		const auto now = system_time();

		std::unordered_map<team_id, bigtime_t> team_current_user;

		int32 team_cookie = 0;
		team_info team {};

		while (get_next_team_info(&team_cookie, &team) == B_OK) {
			if (team.team == B_SYSTEM_TEAM)
				continue;

			team_usage_info usage {};

			if (get_team_usage_info(
				team.team, B_TEAM_USAGE_SELF, &usage) == B_OK)
				team_current_user[team.team] = usage.user_time;
		}

		if (old_time == 0) {
			old_time = now;

			for (uint32 i = 0; i < system.cpu_count; i++)
				core_old_active.at(i) = cpus.at(i).active_time;

			team_old_user = std::move(team_current_user);

			return current_cpu;
		}

		const auto elapsed = now - old_time;
		old_time = now;

		if (elapsed <= 0)
			return current_cpu;

		long long active_sum = 0;

		bigtime_t user_sum = 0;

		for (const auto& [team_id, user_time] : team_current_user) {
			const auto previous = team_old_user.find(team_id);

			if (previous != team_old_user.end())
				user_sum += std::max<bigtime_t>(
					0, user_time - previous->second);
		}

		team_old_user = std::move(team_current_user);

		for (uint32 i = 0; i < system.cpu_count; i++) {
			const auto active = cpus.at(i).active_time;
			const auto delta = std::max<bigtime_t>(
				0, active - core_old_active.at(i));

			core_old_active.at(i) = active;
			active_sum += delta;

			const auto percent = std::clamp(
				static_cast<long long>(std::llround(
					static_cast<double>(delta) * 100.0 /
					static_cast<double>(elapsed))),
				0LL, 100LL);

			current_cpu.core_percent.at(i).push_back(percent);

			if (current_cpu.core_percent.at(i).size() > 40)
				current_cpu.core_percent.at(i).pop_front();
		}

		const auto capacity =
			static_cast<double>(elapsed) *
			static_cast<double>(system.cpu_count);

		const auto total = std::clamp(
			static_cast<long long>(std::llround(
				static_cast<double>(active_sum) * 100.0 / capacity)),
			0LL, 100LL);

		const auto user = std::clamp(
			static_cast<long long>(std::llround(
				static_cast<double>(user_sum) * 100.0 / capacity)),
			0LL, total);

		const auto system_percent = std::clamp(
			total - user,
			0LL, 100LL);

		current_cpu.cpu_percent.at("total").push_back(total);
		current_cpu.cpu_percent.at("user").push_back(user);
		current_cpu.cpu_percent.at("system").push_back(system_percent);

		for (const auto& field : {"total", "user", "system"}) {
			while (current_cpu.cpu_percent.at(field).size() > 40)
				current_cpu.cpu_percent.at(field).pop_front();
		}

		return current_cpu;
	}
}

namespace Mem {
	bool has_swap = false;
	int disk_ios = 0;

	mem_info current_mem {};

	uint64_t get_totalMem() {
		system_info system {};

		if (get_system_info(&system) != B_OK)
			return 0;

		return static_cast<uint64_t>(system.max_pages) *
		       static_cast<uint64_t>(Shared::page_size);
	}

	auto collect(bool no_update) -> mem_info& {
		if (Runner::stopping or
		    (no_update and not current_mem.percent.at("used").empty()))
			return current_mem;

		system_info system {};
		if (get_system_info(&system) != B_OK) {
			Logger::error("Mem::collect() -> get_system_info() failed");
			return current_mem;
		}

		auto& mem = current_mem;

		const auto total =
			static_cast<uint64_t>(system.max_pages) *
			static_cast<uint64_t>(Shared::page_size);

		const auto used =
			static_cast<uint64_t>(system.used_pages) *
			static_cast<uint64_t>(Shared::page_size);

		const auto cached =
			static_cast<uint64_t>(system.cached_pages) *
			static_cast<uint64_t>(Shared::page_size);

		const auto available = total > used ? total - used : 0;
		const auto free = available > cached ? available - cached : 0;

		mem.stats.at("used") = used;
		mem.stats.at("available") = available;
		mem.stats.at("cached") = cached;
		mem.stats.at("free") = free;

		const auto swap_total =
			static_cast<uint64_t>(system.max_swap_pages) *
			static_cast<uint64_t>(Shared::page_size);

		const auto swap_free =
			static_cast<uint64_t>(system.free_swap_pages) *
			static_cast<uint64_t>(Shared::page_size);

		const auto swap_used =
			swap_total > swap_free ? swap_total - swap_free : 0;

		mem.stats.at("swap_total") = swap_total;
		mem.stats.at("swap_used") = swap_used;
		mem.stats.at("swap_free") = swap_free;

		if (Config::getB("show_swap") and swap_total > 0) {
			for (const auto& name : swap_names) {
				const auto percent = static_cast<long long>(std::llround(
					static_cast<double>(mem.stats.at(name)) * 100.0 /
					static_cast<double>(swap_total)));

				mem.percent.at(name).push_back(percent);

				while (mem.percent.at(name).size() > 40)
					mem.percent.at(name).pop_front();
			}

			has_swap = true;
		}
		else {
			has_swap = false;
		}

		disk_ios = 0;

		if (Config::getB("show_disks")) {
			auto& disks = mem.disks;
			auto& disks_filter = Config::getS("disks_filter");
			vector<string> filter;
			bool filter_exclude = false;
			vector<string> found;

			if (not disks_filter.empty()) {
				filter = ssplit(disks_filter);
				if (filter.at(0).starts_with("exclude=")) {
					filter_exclude = true;
					filter.at(0) = filter.at(0).substr(8);
				}
			}

			int32 cookie = 0;
			dev_t dev;

			while ((dev = next_dev(&cookie)) >= 0) {
				fs_info info {};
				if (fs_stat_dev(dev, &info) != B_OK)
					continue;

				if (info.block_size <= 0 or info.total_blocks <= 0)
					continue;

				node_ref ref {};
				ref.device = info.dev;
				ref.node = info.root;

				BDirectory directory(&ref);
				BEntry entry;
				BPath path;

				if (directory.InitCheck() != B_OK or
				    directory.GetEntry(&entry) != B_OK or
				    entry.GetPath(&path) != B_OK)
					continue;

				const string mountpoint = path.Path();

				if (not filter.empty()) {
					const bool match = v_contains(filter, mountpoint);
					if ((filter_exclude and match) or
					    (not filter_exclude and not match))
						continue;
				}

				found.push_back(mountpoint);

				auto& disk = disks[mountpoint];
				disk.dev = info.device_name;
				disk.name = mountpoint == "/" ? "root" : mountpoint;
				disk.fstype = info.fsh_name;

				disk.total =
					static_cast<int64_t>(info.total_blocks) *
					static_cast<int64_t>(info.block_size);
				disk.free =
					static_cast<int64_t>(info.free_blocks) *
					static_cast<int64_t>(info.block_size);
				disk.used = std::max<int64_t>(0, disk.total - disk.free);

				if (disk.total > 0) {
					disk.used_percent = static_cast<int>(
						std::llround(static_cast<double>(disk.used) * 100.0 /
						             static_cast<double>(disk.total)));
					disk.free_percent = 100 - disk.used_percent;
				}
			}

			if (Config::getB("swap_disk") and has_swap) {
				found.push_back("swap");

				auto& swap = disks["swap"];
				swap.name = "swap";
				swap.total = mem.stats.at("swap_total");
				swap.used = mem.stats.at("swap_used");
				swap.free = mem.stats.at("swap_free");
				swap.used_percent = mem.percent.at("swap_used").back();
				swap.free_percent = mem.percent.at("swap_free").back();
			}

			for (auto it = disks.begin(); it != disks.end();) {
				if (not v_contains(found, it->first))
					it = disks.erase(it);
				else
					++it;
			}

			mem.disks_order = std::move(found);
		}
		else {
			mem.disks.clear();
			mem.disks_order.clear();
		}

		for (const auto& name : mem_names) {
			const auto percent = total > 0
				? static_cast<long long>(std::llround(
					static_cast<double>(mem.stats.at(name)) * 100.0 /
					static_cast<double>(total)))
				: 0LL;

			mem.percent.at(name).push_back(percent);

			while (mem.percent.at(name).size() > 40)
				mem.percent.at(name).pop_front();
		}

		return mem;
	}
}

namespace Net {
	std::unordered_map<string, net_info> current_net;
	net_info empty_net {};
	vector<string> interfaces;
	string selected_iface;
	std::unordered_map<string, uint64_t> graph_max = {
		{"download", {}},
		{"upload", {}}
	};
	std::unordered_map<string, std::array<int, 2>> max_count = {
		{"download", {}},
		{"upload", {}}
	};
	bool rescale = true;
	uint64_t timestamp = 0;

	auto collect(bool no_update) -> net_info& {
		auto& net = current_net;
		const auto& config_iface = Config::getS("net_iface");
		const auto net_sync = Config::getB("net_sync");
		const auto net_auto = Config::getB("net_auto");
		const auto new_timestamp = time_ms();

		if (not no_update) {
			interfaces.clear();

			// Get interface names, link state and cumulative byte counters.
			BNetworkRoster& roster = BNetworkRoster::Default();
			uint32 cookie = 0;
			BNetworkInterface interface;

			while (roster.GetNextInterface(&cookie, interface) == B_OK) {
				const string iface = interface.Name();
				ifreq_stats stats {};

				if (interface.GetStats(stats) != B_OK)
					continue;

				interfaces.push_back(iface);
				net[iface].connected = interface.HasLink();

				const uint64_t values[2] = {
					stats.receive.bytes,
					stats.send.bytes
				};

				for (int i = 0; i < 2; i++) {
					const string dir = i == 0 ? "download" : "upload";
					auto& saved_stat = net.at(iface).stat.at(dir);
					auto& bandwidth = net.at(iface).bandwidth.at(dir);
					const auto val = values[i];

					if (saved_stat.last == 0 or timestamp == 0) {
						saved_stat.last = val;
						saved_stat.speed = 0;
						saved_stat.total = val;
					} else {
						if (val < saved_stat.last) {
							saved_stat.rollover += saved_stat.last;
							saved_stat.last = 0;
						}

						if (std::cmp_greater(
							    static_cast<unsigned long long>(saved_stat.rollover) +
							        static_cast<unsigned long long>(val),
							    std::numeric_limits<uint64_t>::max())) {
							saved_stat.rollover = 0;
							saved_stat.last = 0;
						}

						const auto elapsed = new_timestamp - timestamp;

						saved_stat.speed = elapsed > 0
							? std::llround(
								static_cast<double>(val - saved_stat.last) /
								(static_cast<double>(elapsed) / 1000.0))
							: 0;

						if (saved_stat.speed > saved_stat.top)
							saved_stat.top = saved_stat.speed;

						if (saved_stat.offset > val + saved_stat.rollover)
							saved_stat.offset = 0;

						saved_stat.total =
							(val + saved_stat.rollover) - saved_stat.offset;
						saved_stat.last = val;
					}

					bandwidth.push_back(saved_stat.speed);

					while (bandwidth.size() > 40)
						bandwidth.pop_front();

					if (net_auto and selected_iface == iface) {
						if (saved_stat.speed > graph_max[dir]) {
							++max_count[dir][0];
							if (max_count[dir][1] > 0)
								--max_count[dir][1];
						} else if (
							graph_max[dir] > (10U << 10) and
							saved_stat.speed < graph_max[dir] / 10) {
							++max_count[dir][1];
							if (max_count[dir][0] > 0)
								--max_count[dir][0];
						}
					}
				}
			}

			// Get IPv4 and IPv6 addresses.
			IfAddrsPtr if_addrs {};

			if (if_addrs.get_status() == 0) {
				static_assert(INET6_ADDRSTRLEN >= INET_ADDRSTRLEN);
				char ip[INET6_ADDRSTRLEN];

				for (auto* ifa = if_addrs.get();
				     ifa != nullptr;
				     ifa = ifa->ifa_next) {
					if (ifa->ifa_addr == nullptr)
						continue;

					const string iface = ifa->ifa_name;

					if (not net.contains(iface))
						continue;

					const auto family = ifa->ifa_addr->sa_family;

					if (family == AF_INET and net[iface].ipv4.empty()) {
						if (inet_ntop(
							    AF_INET,
							    &reinterpret_cast<sockaddr_in*>(
								    ifa->ifa_addr)->sin_addr,
							    ip,
							    sizeof(ip)) != nullptr)
							net[iface].ipv4 = ip;
					} else if (
						family == AF_INET6 and
						net[iface].ipv6.empty()) {
						if (inet_ntop(
							    AF_INET6,
							    &reinterpret_cast<sockaddr_in6*>(
								    ifa->ifa_addr)->sin6_addr,
							    ip,
							    sizeof(ip)) != nullptr)
							net[iface].ipv6 = ip;
					}
				}
			}

			// Remove interfaces which disappeared.
			for (auto it = net.begin(); it != net.end();) {
				if (not v_contains(interfaces, it->first))
					it = net.erase(it);
				else
					++it;
			}

			timestamp = new_timestamp;
		}

		if (net.empty())
			return empty_net;

		// Pick an interface if the configured/current one is unavailable.
		if (selected_iface.empty() or
		    not v_contains(interfaces, selected_iface)) {
			max_count["download"][0] = 0;
			max_count["download"][1] = 0;
			max_count["upload"][0] = 0;
			max_count["upload"][1] = 0;

			redraw = true;

			if (net_auto)
				rescale = true;

			if (not config_iface.empty() and
			    v_contains(interfaces, config_iface)) {
				selected_iface = config_iface;
			} else {
				auto sorted_interfaces = interfaces;

				std::ranges::sort(
					sorted_interfaces,
					[&](const auto& a, const auto& b) {
						return net.at(a).stat.at("download").total +
							       net.at(a).stat.at("upload").total >
						       net.at(b).stat.at("download").total +
							       net.at(b).stat.at("upload").total;
					});

				selected_iface.clear();

				for (const auto& iface : sorted_interfaces) {
					if (net.at(iface).connected) {
						selected_iface = iface;
						break;
					}
				}

				if (selected_iface.empty() and
				    not sorted_interfaces.empty())
					selected_iface = sorted_interfaces.front();
			}
		}

		if (selected_iface.empty())
			return empty_net;

		// Maintain btop's automatic graph scaling.
		if (net_auto) {
			bool sync = false;

			for (const string dir : {"download", "upload"}) {
				for (const int sel : {0, 1}) {
					if (rescale or max_count[dir][sel] >= 5) {
						const auto& bandwidth =
							net.at(selected_iface).bandwidth.at(dir);

						const long long avg_speed =
							bandwidth.size() > 5
							? std::accumulate(
								  bandwidth.rbegin(),
								  bandwidth.rbegin() + 5,
								  0LL) /
								  5
							: net.at(selected_iface).stat.at(dir).speed;

						graph_max[dir] = std::max(
							static_cast<uint64_t>(
								avg_speed * (sel == 0 ? 1.3 : 3.0)),
							static_cast<uint64_t>(10U << 10));

						max_count[dir][0] = 0;
						max_count[dir][1] = 0;
						redraw = true;

						if (net_sync)
							sync = true;

						break;
					}
				}

				if (sync) {
					const string other =
						dir == "upload" ? "download" : "upload";

					graph_max[other] = graph_max[dir];
					max_count[other][0] = 0;
					max_count[other][1] = 0;
					break;
				}
			}
		}

		rescale = false;
		return net.at(selected_iface);
	}
}

namespace Proc {
	vector<proc_info> current_procs;
	std::unordered_map<string, string> uid_user;
	string current_sort;
	string current_filter;
	bool current_rev = false;
	bool is_tree_mode = false;

	int collapse = -1;
	int expand = -1;
	int toggle_children = -1;
	int collapse_all = -1;

	std::atomic<int> numpids = 0;
	int filter_found = 0;

	detail_container detailed;
	static std::unordered_set<size_t> dead_procs;
	static bigtime_t last_collect_time = 0;

	string get_status(char state) {
		switch (state) {
			case 'R':
				return "Running";
			case 'S':
				return "Sleeping";
			case 'T':
				return "Stopped";
			case 'W':
				return "Waiting";
			case 'X':
				return "Dead";
			default:
				return "Unknown";
		}
	}

	char team_state(
		bool running,
		bool ready,
		bool waiting,
		bool sleeping,
		bool suspended) {
		if (running)
			return 'R';
		if (ready)
			return 'R';
		if (waiting)
			return 'W';
		if (sleeping)
			return 'S';
		if (suspended)
			return 'T';

		return '?';

	}
	void _collect_details(const size_t pid, vector<proc_info>& procs) {
		if (pid != detailed.last_pid) {
			detailed = {};
			detailed.last_pid = pid;
			detailed.skip_smaps = true;
		}

		auto p_info = std::ranges::find(procs, pid, &proc_info::pid);

		if (p_info == procs.end())
			return;

		detailed.entry = *p_info;

		if (not Config::getB("proc_per_core"))
			detailed.entry.cpu_p *= Shared::coreCount;

		detailed.cpu_percent.push_back(
			std::clamp(
				static_cast<long long>(
					std::llround(detailed.entry.cpu_p)),
				0LL,
				100LL));

		while (detailed.cpu_percent.size() > 40)
			detailed.cpu_percent.pop_front();

		const auto now = std::time(nullptr);

		if (detailed.entry.state != 'X') {
			const auto elapsed =
				now > static_cast<std::time_t>(detailed.entry.cpu_s)
					? now - static_cast<std::time_t>(detailed.entry.cpu_s)
					: 0;

			detailed.elapsed = sec_to_dhms(elapsed);
		} else {
			detailed.elapsed = sec_to_dhms(detailed.entry.death_time);
		}

		if (detailed.elapsed.size() > 8)
			detailed.elapsed.resize(detailed.elapsed.size() - 3);

		if (detailed.parent.empty()) {
			auto parent = std::ranges::find(
				procs,
				detailed.entry.ppid,
				&proc_info::pid);

			if (parent != procs.end())
				detailed.parent = parent->name;
		}

		detailed.status = get_status(detailed.entry.state);

		detailed.mem_bytes.push_back(detailed.entry.mem);
		detailed.memory = floating_humanizer(detailed.entry.mem);

		if (detailed.first_mem == -1 or
		    detailed.first_mem < detailed.mem_bytes.back() / 2 or
		    detailed.first_mem > detailed.mem_bytes.back() * 4) {
			detailed.first_mem = std::min(
				static_cast<uint64_t>(detailed.mem_bytes.back() * 2),
				Mem::get_totalMem());

			redraw = true;
		}

		while (detailed.mem_bytes.size() > 40)
			detailed.mem_bytes.pop_front();
	}
	auto collect(bool no_update) -> vector<proc_info>& {
		const auto& sorting = Config::getS("proc_sorting");
		const auto reverse = Config::getB("proc_reversed");
		const auto& filter = Config::getS("proc_filter");
		const auto per_core = Config::getB("proc_per_core");
		const auto tree = Config::getB("proc_tree");
		const auto show_detailed = Config::getB("show_detailed");
		const auto pause_proc_list = Config::getB("pause_proc_list");
		const size_t detailed_pid = Config::getI("detailed_pid");

		bool should_filter = current_filter != filter;
		if (should_filter)
			current_filter = filter;

		const bool sorted_change =
			sorting != current_sort or
			reverse != current_rev or
			should_filter;

		const bool tree_mode_change = tree != is_tree_mode;

		if (sorted_change) {
			current_sort = sorting;
			current_rev = reverse;
		}

		if (tree_mode_change)
			is_tree_mode = tree;

		bool got_detailed = false;
		static vector<size_t> found;

		if (no_update and not current_procs.empty()) {
			if (show_detailed and detailed_pid != detailed.last_pid)
				_collect_details(detailed_pid, current_procs);
		} else {
			should_filter = true;
			found.clear();

			const auto now = system_time();
			const auto elapsed =
				last_collect_time > 0 ? now - last_collect_time : 0;

			last_collect_time = now;

			const auto unix_now = std::time(nullptr);

			int32 team_cookie = 0;
			team_info team {};

			while (get_next_team_info(&team_cookie, &team) == B_OK) {
				if (team.team < 1)
					continue;

				const size_t pid = static_cast<size_t>(team.team);
				found.push_back(pid);

				bool no_cache = false;

				auto find_old = std::ranges::find(
					current_procs,
					pid,
					&proc_info::pid);

				if (find_old == current_procs.end()) {
					if (pause_proc_list)
						continue;

					current_procs.push_back({pid});
					find_old = current_procs.end() - 1;
					no_cache = true;
				} else if (dead_procs.contains(pid)) {
					continue;
				}

				auto& proc = *find_old;

				if (no_cache) {
					proc.name = team.name;

					const auto args_len =
						strnlen(team.args, sizeof(team.args));

					if (args_len > 0)
						proc.cmd.assign(team.args, args_len);

					if (proc.cmd.empty())
						proc.cmd = proc.name;

					proc.ppid = team.parent > 0
						? static_cast<uint64_t>(team.parent)
						: 0;

					if (team.start_time > 0 and now >= team.start_time) {
						const auto age_seconds =
							static_cast<uint64_t>(
								(now - team.start_time) / 1'000'000);

						proc.cpu_s =
							unix_now > static_cast<std::time_t>(age_seconds)
								? static_cast<uint64_t>(
									unix_now - age_seconds)
								: 0;
					} else {
						proc.cpu_s = static_cast<uint64_t>(unix_now);
					}

					const string uid = std::to_string(team.uid);

					if (auto cached = uid_user.find(uid);
					    cached != uid_user.end()) {
						proc.user = cached->second;
					} else {
						if (const auto* pwd = getpwuid(team.uid);
						    pwd != nullptr)
							proc.user = pwd->pw_name;
						else
							proc.user = uid;

						uid_user[uid] = proc.user;
					}
				}

				uint64_t resident = 0;

				ssize_t area_cookie = 0;
				area_info area {};

				while (get_next_area_info(
					       team.team, &area_cookie, &area) == B_OK) {
					resident += area.ram_size;
				}

				uint64_t cpu_time = proc.cpu_t;

				team_usage_info usage {};
				if (get_team_usage_info(
					team.team, B_TEAM_USAGE_SELF, &usage) == B_OK) {
					cpu_time =
						static_cast<uint64_t>(usage.user_time) +
						static_cast<uint64_t>(usage.kernel_time);
				}

				bool running = false;
				bool ready = false;
				bool waiting = false;
				bool sleeping = false;
				bool suspended = false;

				int32 thread_cookie = 0;
				thread_info thread {};

				while (get_next_thread_info(
					       team.team, &thread_cookie, &thread) == B_OK) {
					switch (thread.state) {
						case B_THREAD_RUNNING:
							running = true;
							break;
						case B_THREAD_READY:
							ready = true;
							break;
						case B_THREAD_RECEIVING:
						case B_THREAD_WAITING:
							waiting = true;
							break;
						case B_THREAD_ASLEEP:
							sleeping = true;
							break;
						case B_THREAD_SUSPENDED:
							suspended = true;
							break;
						default:
							break;
					}
				}

				proc.mem = resident;
				proc.threads = static_cast<size_t>(team.thread_count);
				proc.p_nice = 0;
				proc.state = team_state(
					running, ready, waiting, sleeping, suspended);

                                const auto old_cpu_time = proc.cpu_t;

                                if (team.team != B_SYSTEM_TEAM and
                                    not no_cache and
                                    elapsed > 0 and
                                    cpu_time >= old_cpu_time) {
					const auto delta = cpu_time - old_cpu_time;

					double cpu_percent =
						static_cast<double>(delta) * 100.0 /
						static_cast<double>(elapsed);

					if (not per_core and Shared::coreCount > 0)
						cpu_percent /= Shared::coreCount;

					const double maximum =
						per_core
							? 100.0 * Shared::coreCount
							: 100.0;

					proc.cpu_p = std::clamp(
						cpu_percent, 0.0, maximum);
				} else {
					proc.cpu_p = 0.0;
				}

				proc.cpu_t = cpu_time;

				if (team.team == B_SYSTEM_TEAM) {
					proc.cpu_c = 0.0;
				} else if (proc.cpu_s > 0) {
					const auto age =
						unix_now -
						static_cast<std::time_t>(proc.cpu_s);

					if (age > 0) {
						double cumulative =
							static_cast<double>(cpu_time) /
							1'000'000.0 /
							static_cast<double>(age) *
							100.0;

						if (not per_core and Shared::coreCount > 0)
							cumulative /= Shared::coreCount;

						proc.cpu_c = cumulative;
					}
				}

				if (show_detailed and
				    not got_detailed and
				    proc.pid == detailed_pid)
					got_detailed = true;
			}
			if (not pause_proc_list) {
				auto eraser = std::ranges::remove_if(
					current_procs,
					[&](const auto& proc) {
						return not v_contains(found, proc.pid);
					});

				current_procs.erase(
					eraser.begin(),
					eraser.end());

				if (not dead_procs.empty())
					dead_procs.clear();
			} else {
				const bool keep_dead_proc_usage =
					Config::getB("keep_dead_proc_usage");

				for (auto& proc : current_procs) {
					if (std::ranges::find(found, proc.pid) ==
					    found.end()) {
						if (proc.state != 'X') {
							const auto now_time =
								std::time(nullptr);

							proc.death_time =
								now_time >
										static_cast<std::time_t>(
											proc.cpu_s)
									? now_time -
										static_cast<std::time_t>(
											proc.cpu_s)
									: 0;
						}

						proc.state = 'X';
						dead_procs.emplace(proc.pid);

						if (not keep_dead_proc_usage) {
							proc.cpu_p = 0.0;
							proc.mem = 0;
						}
					}
				}
			}

			if (show_detailed and got_detailed) {
				_collect_details(detailed_pid, current_procs);
			} else if (
				show_detailed and
				not got_detailed and
				detailed.status != "Dead") {
				detailed.status = "Dead";
				redraw = true;
			}
		}
		if (should_filter) {
			filter_found = 0;

			for (auto& proc : current_procs) {
				if (not tree and not filter.empty()) {
					if (not matches_filter(proc, filter)) {
						proc.filtered = true;
						filter_found++;
					} else {
						proc.filtered = false;
					}
				} else {
					proc.filtered = false;
				}
			}
		}

		if (sorted_change or
		    tree_mode_change or
		    (not no_update and not pause_proc_list)) {
			proc_sorter(
				current_procs,
				sorting,
				reverse,
				tree);
		}
		if (tree and
		    not current_procs.empty() and
		    (not no_update or
		     should_filter or
		     sorted_change)) {
			bool locate_selection = false;

			if (toggle_children != -1) {
				auto collapser = std::ranges::find(
					current_procs,
					toggle_children,
					&proc_info::pid);

				if (collapser != current_procs.end()) {
					for (auto& proc : current_procs) {
						if (proc.ppid == collapser->pid) {
							auto child = std::ranges::find(
								current_procs,
								proc.pid,
								&proc_info::pid);

							if (child != current_procs.end())
								child->collapsed =
									not child->collapsed;
						}
					}

					if (Config::ints.at("proc_selected") > 0)
						locate_selection = true;
				}

				toggle_children = -1;
			}

			if (auto find_pid =
				    collapse != -1 ? collapse : expand;
			    find_pid != -1) {
				auto collapser = std::ranges::find(
					current_procs,
					find_pid,
					&proc_info::pid);

				if (collapser != current_procs.end()) {
					if (collapse == expand)
						collapser->collapsed =
							not collapser->collapsed;
					else if (collapse > -1)
						collapser->collapsed = true;
					else if (expand > -1)
						collapser->collapsed = false;

					if (Config::ints.at("proc_selected") > 0)
						locate_selection = true;
				}

				collapse = expand = -1;
			}

			if (collapse_all != -1) {
				toggle_tree_collapse(current_procs);
				collapse_all = -1;

				if (Config::ints.at("proc_selected") > 0)
					locate_selection = true;
			}
			if (should_filter or not filter.empty())
				filter_found = 0;

			vector<tree_proc> tree_procs;
			tree_procs.reserve(current_procs.size());

			if (not pause_proc_list) {
				for (auto& proc : current_procs) {
					if (not v_contains(found, proc.ppid))
						proc.ppid = 0;
				}
			}

			std::ranges::stable_sort(
				current_procs,
				std::ranges::less {},
				&proc_info::ppid);

			_auto_collapse_oversized(
				current_procs,
				tree_mode_change);

			for (auto& proc :
			     std::ranges::equal_range(
				     current_procs,
				     current_procs.front().ppid,
				     std::ranges::less {},
				     &proc_info::ppid)) {
				_tree_gen(
					proc,
					current_procs,
					tree_procs,
					0,
					false,
					filter,
					false,
					no_update,
					should_filter);
			}

			int index = 0;

			tree_sort(
				tree_procs,
				sorting,
				reverse,
				pause_proc_list and
					not (sorted_change or tree_mode_change),
				index,
				current_procs.size());

			for (auto iter = tree_procs.begin();
			     iter != tree_procs.end();
			     ++iter) {
				_collect_prefixes(
					*iter,
					iter == tree_procs.end() - 1);
			}

			std::ranges::stable_sort(
				current_procs,
				std::ranges::less {},
				&proc_info::tree_index);
			if (locate_selection) {
				auto selected = std::ranges::find(
					current_procs,
					Proc::selected_pid,
					&proc_info::pid);

				if (selected != current_procs.end()) {
					const int loc = selected->tree_index;

					if (Config::ints.at("proc_start") >= loc or
					    Config::ints.at("proc_start") <=
						    loc - Proc::select_max) {
						Config::ints.at("proc_start") =
							std::max(0, loc - 1);
					}

					Config::ints.at("proc_selected") =
						loc -
						Config::ints.at("proc_start") +
						1;
				}
			}
		}

		numpids =
			static_cast<int>(current_procs.size()) -
			filter_found;

		return current_procs;
	}
}  // namespace Proc

namespace Shared {
	long coreCount = 1;
	long page_size = B_PAGE_SIZE;
	long clk_tck = 100;

	void init() {
		system_info system {};

		if (get_system_info(&system) != B_OK or system.cpu_count == 0)
			throw std::runtime_error("Failed to get Haiku system information");

		coreCount = system.cpu_count;

		Cpu::current_cpu.core_percent.resize(coreCount);
		Cpu::current_cpu.temp.resize(coreCount + 1);
		Cpu::core_old_active.resize(coreCount);

		Cpu::core_mapping = Cpu::get_core_mapping();
		Cpu::cpuName = Cpu::get_cpuName();

		Cpu::collect();
		Mem::collect();
	}
}

namespace Tools {
	double system_uptime() {
		return static_cast<double>(system_time()) / 1'000'000.0;
	}
}
