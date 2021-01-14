#pragma once

#include <mysql++11.h>

#include "IDatabase.h"

#pragma comment(lib, "mysqlclient.lib")

class MySql : public IDatabase
{
public:
	explicit MySql(std::string server, std::string username, std::string password, std::string db_name, const unsigned int port,
		std::string table_players, std::string table_groups, std::string table_tribes)
		: table_players_(move(table_players)), table_tribes_(move(table_tribes)),
		table_groups_(move(table_groups))
	{
		try
		{
			daotk::mysql::connect_options options;
			options.server = move(server);
			options.username = move(username);
			options.password = move(password);
			options.dbname = move(db_name);
			options.autoreconnect = true;
			options.timeout = 30;
			options.port = port;

			bool result = db_.open(options);
			if (!result)
			{
				Log::GetLog()->critical("Failed to open connection!");
				return;
			}

			result = db_.query(fmt::format("CREATE TABLE IF NOT EXISTS {} ("
				"Id INT NOT NULL AUTO_INCREMENT,"
				"SteamId BIGINT(11) NOT NULL,"
				"PermissionGroups VARCHAR(256) NOT NULL DEFAULT 'Default,',"
				"TimedPermissionGroups VARCHAR(256) NOT NULL DEFAULT '',"
				"PRIMARY KEY(Id),"
				"UNIQUE INDEX SteamId_UNIQUE (SteamId ASC));", table_players_));
			result = db_.query(fmt::format("CREATE TABLE IF NOT EXISTS {} ("
				"Id INT NOT NULL AUTO_INCREMENT,"
				"TribeId BIGINT(11) NOT NULL,"
				"PermissionGroups VARCHAR(256) NOT NULL DEFAULT '',"
				"TimedPermissionGroups VARCHAR(256) NOT NULL DEFAULT '',"
				"PRIMARY KEY(Id),"
				"UNIQUE INDEX SteamId_UNIQUE (TribeId ASC));", table_tribes_));
			result |= db_.query(fmt::format("CREATE TABLE IF NOT EXISTS {} ("
				"Id INT NOT NULL AUTO_INCREMENT,"
				"GroupName VARCHAR(128) NOT NULL,"
				"Permissions VARCHAR(768) NOT NULL DEFAULT '',"
				"PRIMARY KEY(Id),"
				"UNIQUE INDEX GroupName_UNIQUE (GroupName ASC));", table_groups_));

			// Add default groups

			result |= db_.query(fmt::format("INSERT INTO {} (GroupName, Permissions)"
				"SELECT 'Admins', '*,'"
				"WHERE NOT EXISTS(SELECT 1 FROM {} WHERE GroupName = 'Admins');",
				table_groups_,
				table_groups_));
			result |= db_.query(fmt::format("INSERT INTO {} (GroupName)"
				"SELECT 'Default'"
				"WHERE NOT EXISTS(SELECT 1 FROM {} WHERE GroupName = 'Default');",
				table_groups_,
				table_groups_));

			upgradeDatabase(db_name);

			if (!result)
			{
				Log::GetLog()->critical("({} {}) Failed to create table!", __FILE__, __FUNCTION__);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->critical("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}
	}

	bool AddPlayer(uint64 steam_id) override
	{
		try
		{
			if (db_.query(fmt::format("INSERT INTO {} (SteamId) VALUES ('{}');", table_players_, steam_id, "Default,")))
			{
				std::lock_guard<std::mutex> lg(playersMutex);
				permissionPlayers[steam_id] = CachedPermission("Default,", "");
				return true;
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return false;
		}

		return false;
	}

	bool IsPlayerExists(uint64 steam_id) override
	{
		return permissionPlayers.count(steam_id) > 0;
	}

	bool IsGroupExists(const FString& group) override
	{
		return permissionGroups.count(group.ToString()) > 0;
	}

	TArray<FString> GetPlayerGroups(uint64 steam_id) override
	{
		TArray<FString> groups;
		auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (permissionPlayers.count(steam_id) > 0)
		{
			groups = permissionPlayers[steam_id].getGroups(nowSecs);
		}

		return groups;
	}

	TArray<FString> GetGroupPermissions(const FString& group) override
	{
		if (group.IsEmpty())
			return {};

		TArray<FString> permissions;

		if (permissionGroups.count(group.ToString()) > 0)
		{
			FString permissions_fstr(permissionGroups[group.ToString()]);
			permissions_fstr.ParseIntoArray(permissions, L",", true);
		}

		return permissions;
	}

	TArray<FString> GetAllGroups() override
	{
		TArray<FString> all_groups;

		for (auto& group : permissionGroups)
		{
			all_groups.Add(group.second.c_str());
		}

		return all_groups;
	}

	TArray<uint64> GetGroupMembers(const FString& group) override
	{
		TArray<uint64> members;

		for (auto& players : permissionPlayers)
		{
			if (Permissions::IsPlayerInGroup(players.first, group))
				members.Add(players.first);
		}

		return members;
	}

	std::optional<std::string> AddPlayerToGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id))
			AddPlayer(steam_id);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		if (Permissions::IsPlayerInGroup(steam_id, group))
			return "Player was already added";

		try
		{
			const bool res = db_.query(fmt::format(
				"UPDATE {} SET PermissionGroups = concat(PermissionGroups, '{},') WHERE SteamId = {};",
				table_players_, group.ToString(), steam_id));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(playersMutex);
				permissionPlayers[steam_id].Groups.Add(group);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemovePlayerFromGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id) || !IsGroupExists(group))
			return "Player or group does not exist";

		if (!Permissions::IsPlayerInGroup(steam_id, group))
			return "Player is not in group";

		TArray<FString> groups = GetPlayerGroups(steam_id);

		FString new_groups;

		for (const FString& current_group : groups)
		{
			if (current_group != group)
				new_groups += current_group + ",";
		}

		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET PermissionGroups = '{}' WHERE SteamId = {};",
				table_players_, new_groups.ToString(), steam_id));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(playersMutex);
				permissionPlayers[steam_id].Groups.Remove(group);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddPlayerToTimedGroup(uint64 steam_id, const FString& group, int secs, int delaySecs) override
	{
		if (!IsPlayerExists(steam_id))
			AddPlayer(steam_id);

		if (!IsGroupExists(group))
			return  "Group does not exist";
		
		TArray<TimedGroup> groups;
		if (permissionPlayers.count(steam_id) > 0)
		{
			groups = permissionPlayers[steam_id].TimedGroups;
		}
		for (int32 Index = groups.Num() - 1; Index >= 0; --Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName.Equals(group)) {
				groups.RemoveAt(Index);
				continue;
			}
		}
		if (Permissions::IsPlayerInGroup(steam_id, group))
			return "Player is already permanetly in this group.";

		long long ExpireAtSecs = 0;
		long long delayUntilSecs = 0;
		if (delaySecs > 0) {
			delayUntilSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(delaySecs)).time_since_epoch()).count();
		}
		ExpireAtSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(secs)).time_since_epoch()).count();

		groups.Add(TimedGroup{ group, delayUntilSecs, ExpireAtSecs });
		FString new_groups;
		for (const TimedGroup& current_group : groups)
		{
			new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
		}
		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET TimedPermissionGroups = '{}' WHERE SteamId = {};",
				table_players_, new_groups.ToString(), steam_id));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(playersMutex);
				permissionPlayers[steam_id].TimedGroups = groups;
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemovePlayerFromTimedGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id) || !IsGroupExists(group))
			return "Player or group does not exist";

		TArray<TimedGroup> groups = permissionPlayers[steam_id].TimedGroups;

		FString new_groups;

		int32 groupIndex = INDEX_NONE;
		for (int32 Index = 0; Index != groups.Num(); ++Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName != group)
				new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
			else
				groupIndex = Index;
		}
		if (groupIndex == INDEX_NONE)
			return "Player is not in timed group";

		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET TimedPermissionGroups = '{}' WHERE SteamId = {};",
				table_players_, new_groups.ToString(), steam_id));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(playersMutex);
				permissionPlayers[steam_id].TimedGroups.RemoveAt(groupIndex);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddGroup(const FString& group) override
	{
		if (IsGroupExists(group))
			return "Group already exists";

		try
		{
			const bool res = db_.query(fmt::format("INSERT INTO {} (GroupName) VALUES ('{}');", table_groups_,
				group.ToString()));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(groupsMutex);
				permissionGroups[group.ToString()] = "";
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveGroup(const FString& group) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		// Remove all players from this group

		TArray<uint64> group_members = GetGroupMembers(group);
		for (uint64 player : group_members)
		{
			RemovePlayerFromGroup(player, group);
		}

		// Delete group

		try
		{
			const bool res = db_.query(fmt::format("DELETE FROM {} WHERE GroupName = '{}';", table_groups_,
				group.ToString()));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(groupsMutex);
				permissionGroups.erase(group.ToString());
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> GroupGrantPermission(const FString& group, const FString& permission) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		if (Permissions::IsGroupHasPermission(group, permission))
			return "Group already has this permission";

		try
		{
			const bool res = db_.query(fmt::format(
				"UPDATE {} SET Permissions = concat(Permissions, '{},') WHERE GroupName = '{}';",
				table_groups_, permission.ToString(), group.ToString()));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(groupsMutex);
				std::string groupPermissions = fmt::format("{},{}", permission.ToString(), permissionGroups[group.ToString()]);
				permissionGroups[group.ToString()] = groupPermissions;
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> GroupRevokePermission(const FString& group, const FString& permission) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		if (!Permissions::IsGroupHasPermission(group, permission))
			return "Group does not have this permission";

		TArray<FString> permissions = GetGroupPermissions(group);

		FString new_permissions;

		for (const FString& current_perm : permissions)
		{
			if (current_perm != permission)
				new_permissions += current_perm + ",";
		}

		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET Permissions = '{}' WHERE GroupName = '{}';",
				table_groups_, new_permissions.ToString(), group.ToString()));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(groupsMutex);
				permissionGroups[group.ToString()] = new_permissions.ToString();
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	void Init() override
	{
		auto pGroups = InitGroups();
		groupsMutex.lock();
		permissionGroups = pGroups;
		groupsMutex.unlock();

		auto pPlayers = InitPlayers();
		playersMutex.lock();
		permissionPlayers = pPlayers;
		playersMutex.unlock();

		auto pTribes = InitTribes();
		tribesMutex.lock();
		permissionTribes = pTribes;
		tribesMutex.unlock();
	}

	std::unordered_map<std::string, std::string> InitGroups() override
	{
		std::unordered_map<std::string, std::string> pGroups;

		try
		{
			db_.query(fmt::format("SELECT GroupName, Permissions FROM {};", table_groups_))
				.each([&pGroups](std::string groupName, std::string groupPermissions)
					{
						pGroups[groupName] = groupPermissions;
						return true;
					});
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pGroups;
	}

	std::unordered_map<uint64, CachedPermission> InitPlayers() override
	{
		std::unordered_map<uint64, CachedPermission> pPlayers;

		try
		{
			db_.query(fmt::format("SELECT SteamId, PermissionGroups, TimedPermissionGroups FROM {};", table_players_))
				.each([&pPlayers](uint64 steam_id, std::string groups, std::string timedGroups)
					{
						pPlayers[steam_id] = CachedPermission(FString(groups), FString(timedGroups));
						return true;
					});
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pPlayers;
	}

	bool AddTribe(int tribeId) override
	{
		try
		{
			if (db_.query(fmt::format("INSERT INTO {} (TribeId) VALUES ({});", table_players_, tribeId)))
			{
				std::lock_guard<std::mutex> lg(tribesMutex);
				permissionTribes[tribeId] = CachedPermission("", "");
				return true;
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return false;
		}

		return false;
	}

	bool IsTribeExists(int tribeId) override
	{
		return permissionTribes.count(tribeId) > 0;
	}

	TArray<FString> GetTribeGroups(int tribeId) override
	{
		TArray<FString> groups;
		auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (permissionTribes.count(tribeId) > 0)
		{
			groups = permissionTribes[tribeId].getGroups(nowSecs);
		}

		return groups;
	}

	std::optional<std::string> AddTribeToGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId))
			AddTribe(tribeId);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		if (Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe was already added";

		try
		{
			const bool res = db_.query(fmt::format(
				"UPDATE {} SET PermissionGroups = concat(PermissionGroups, '{},') WHERE TribeId = {};",
				table_players_, group.ToString(), tribeId));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(tribesMutex);
				permissionTribes[tribeId].Groups.Add(group);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveTribeFromGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId) || !IsGroupExists(group))
			return "Tribe or group does not exist";

		if (!Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe is not in group";

		TArray<FString> groups = GetTribeGroups(tribeId);

		FString new_groups;

		for (const FString& current_group : groups)
		{
			if (current_group != group)
				new_groups += current_group + ",";
		}

		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET PermissionGroups = '{}' WHERE TribeId = {};",
				table_players_, new_groups.ToString(), tribeId));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(tribesMutex);
				permissionTribes[tribeId].Groups.Remove(group);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddTribeToTimedGroup(int tribeId, const FString& group, int secs, int delaySecs) override
	{
		if (!IsTribeExists(tribeId))
			AddTribe(tribeId);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		TArray<TimedGroup> groups;
		if (permissionTribes.count(tribeId) > 0)
		{
			groups = permissionTribes[tribeId].TimedGroups;
		}
		for (int32 Index = groups.Num() - 1; Index >= 0; --Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName.Equals(group)) {
				groups.RemoveAt(Index);
				continue;
			}
		}
		if (Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe is already permanetly in this group.";

		long long ExpireAtSecs = 0;
		long long delayUntilSecs = 0;
		if (delaySecs > 0) {
			delayUntilSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(delaySecs)).time_since_epoch()).count();
		}
		ExpireAtSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(secs)).time_since_epoch()).count();

		groups.Add(TimedGroup{ group, delayUntilSecs, ExpireAtSecs });
		FString new_groups;
		for (const TimedGroup& current_group : groups)
		{
			new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
		}
		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET TimedPermissionGroups = '{}' WHERE TribeId = {};",
				table_players_, new_groups.ToString(), tribeId));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(tribesMutex);
				permissionTribes[tribeId].TimedGroups = groups;
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveTribeFromTimedGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId) || !IsGroupExists(group))
			return "Tribe or group does not exist";

		TArray<TimedGroup> groups = permissionTribes[tribeId].TimedGroups;

		FString new_groups;

		int32 groupIndex = INDEX_NONE;
		for (int32 Index = 0; Index != groups.Num(); ++Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName != group)
				new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
			else
				groupIndex = Index;
		}
		if (groupIndex == INDEX_NONE)
			return "Tribe is not in timed group";

		try
		{
			const bool res = db_.query(fmt::format("UPDATE {} SET TimedPermissionGroups = '{}' WHERE TribeId = {};",
				table_players_, new_groups.ToString(), tribeId));
			if (!res)
			{
				return "Unexpected DB error";
			}
			else
			{
				std::lock_guard<std::mutex> lg(tribesMutex);
				permissionTribes[tribeId].TimedGroups.RemoveAt(groupIndex);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::unordered_map<int, CachedPermission> InitTribes() override
	{
		std::unordered_map<int, CachedPermission> pTribes;

		try
		{
			db_.query(fmt::format("SELECT TribeId, PermissionGroups, TimedPermissionGroups FROM {};", table_players_))
				.each([&pTribes](int tribeId, std::string groups, std::string timedGroups)
			{
				pTribes[tribeId] = CachedPermission(FString(groups), FString(timedGroups));
				return true;
			});
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pTribes;
	}

	void upgradeDatabase(std::string db_name) {
		auto query = fmt::format("SELECT IF(count(*) = 1, 'Exist', 'Not Exist') AS result FROM information_schema.columns WHERE table_schema = '{}' AND table_name = '{}' AND column_name = '{}';",
			db_name, table_players_, "TimedPermissionGroups");
		auto selectResult = db_.query(query);
		if (!selectResult)
		{
			Log::GetLog()->critical("({} {}Failed to check Permissions table!", __FILE__, __FUNCTION__);
		}
		else {
			auto exists = selectResult.get_value<std::string>();
			if (exists.c_str() == "Not Exist") {
				auto updateResult = db_.query(fmt::format("ALTER TABLE {} ADD COLUMN TimedPermissionGroups VARCHAR(256) DEFAULT '' AFTER PermissionGroups;", table_players_));
				if (!updateResult)
				{
					Log::GetLog()->critical("({} {})Failed to update Permissions table!", __FILE__, __FUNCTION__);
				}
				else {
					Log::GetLog()->warn("Upgraded Permissions DB Tables.");
				}
			}
		}
	}

private:
	daotk::mysql::connection db_;
	std::string table_players_;
	std::string table_tribes_;
	std::string table_groups_;
};
