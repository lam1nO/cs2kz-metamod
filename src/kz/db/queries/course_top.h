constexpr char sql_getcoursetop[] = R"(
    SELECT t.ID, t.SteamID64, p.Alias, t.RunTime AS PBTime, t.Teleports, t.Created 
        FROM Times t 
        INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID 
        INNER JOIN Maps ON Maps.ID = mc.MapID
        INNER JOIN Players p ON p.SteamID64=t.SteamID64 
    LEFT JOIN Bans b ON b.SteamID64=t.SteamID64 AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP)
        LEFT OUTER JOIN Times t2 ON t2.SteamID64=t.SteamID64 
        AND t2.MapCourseID=t.MapCourseID AND t2.ModeID=t.ModeID
        AND t2.StyleIDFlags=t.StyleIDFlags
		AND (t2.RunTime < t.RunTime OR (t2.RunTime = t.RunTime AND t2.ID < t.ID))
    WHERE t2.ID IS NULL AND b.ID IS NULL AND Maps.Name='%s' AND mc.Name='%s' AND t.ModeID=%d AND t.StyleIDFlags=%llu
        ORDER BY PBTime ASC, t.ID ASC
        LIMIT %d
        OFFSET %d
)";

constexpr char sql_getcoursetoppro[] = R"(
    SELECT t.ID, t.SteamID64, p.Alias, t.RunTime AS PBTime, t.Teleports, t.Created 
        FROM Times t 
        INNER JOIN MapCourses mc ON mc.ID=t.MapCourseID 
        INNER JOIN Maps ON Maps.ID = mc.MapID
        INNER JOIN Players p ON p.SteamID64=t.SteamID64 
    LEFT JOIN Bans b ON b.SteamID64=t.SteamID64 AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP)
        LEFT OUTER JOIN Times t2 ON t2.SteamID64=t.SteamID64 AND t2.MapCourseID=t.MapCourseID 
        AND t2.ModeID=t.ModeID AND t2.StyleIDFlags=t.StyleIDFlags 
        AND (t2.RunTime < t.RunTime OR (t2.RunTime = t.RunTime AND t2.ID < t.ID))
		AND t.Teleports=0 AND t2.Teleports=0
    WHERE t2.ID IS NULL AND b.ID IS NULL AND Maps.Name='%s'
        AND mc.Name='%s' AND t.ModeID=%d AND t.Teleports=0 AND t.StyleIDFlags=%llu
        ORDER BY PBTime ASC, t.ID ASC
        LIMIT %d
        OFFSET %d
)";

constexpr char sql_get_course_personal[] = R"(
    SELECT ID, SteamID64, Alias, RunTime, Teleports, Created, Place FROM (
        SELECT ID, SteamID64, Alias, RunTime, Teleports, Created,
            ROW_NUMBER() OVER (ORDER BY RunTime, ID) AS Place
        FROM (
            SELECT t.ID, t.SteamID64, p.Alias, t.RunTime, t.Teleports, t.Created,
                ROW_NUMBER() OVER (PARTITION BY t.SteamID64 ORDER BY t.RunTime, t.ID) AS PersonalOrder
            FROM Times t
            JOIN MapCourses mc ON mc.ID = t.MapCourseID
            JOIN Maps m ON m.ID = mc.MapID
            JOIN Players p ON p.SteamID64 = t.SteamID64
            WHERE m.Name = '%s' AND mc.Name = '%s' AND t.ModeID = %d AND t.StyleIDFlags = %llu
                AND (%d = 0 OR t.Teleports = 0)
                AND NOT EXISTS (SELECT 1 FROM Bans b WHERE b.SteamID64 = t.SteamID64
                    AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP))
        ) personal WHERE PersonalOrder = 1
    ) ranked WHERE SteamID64 = %llu
)";

constexpr char sql_get_course_styles[] = R"(
    SELECT t.StyleIDFlags, COUNT(*), SUM(CASE WHEN t.Teleports = 0 THEN 1 ELSE 0 END)
        FROM Times t
        INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID
        INNER JOIN Maps m ON m.ID = mc.MapID
        WHERE m.Name = '%s' AND mc.Name = '%s' AND t.ModeID = %d
        AND NOT EXISTS (SELECT 1 FROM Bans b WHERE b.SteamID64 = t.SteamID64
            AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP))
        GROUP BY t.StyleIDFlags
)";

// Caching PBs

constexpr char sql_getsrs[] = R"(
    SELECT x.RunTime, x.MapCourseID, x.ModeID, t.Metadata, x.StyleIDFlags
        FROM Times t
        INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID
        INNER JOIN Maps m ON m.ID = mc.MapID
        INNER JOIN (
            SELECT MIN(t.RunTime) AS RunTime, t.MapCourseID, t.ModeID, t.StyleIDFlags
                FROM Times t
                GROUP BY t.MapCourseID, t.ModeID, t.StyleIDFlags
        ) x ON x.RunTime = t.RunTime AND x.MapCourseID = t.MapCourseID AND x.ModeID = t.ModeID AND x.StyleIDFlags = t.StyleIDFlags
        WHERE m.Name = '%s'
)";

constexpr char sql_getsrspro[] = R"(
    SELECT x.RunTime, x.MapCourseID, x.ModeID, t.Metadata, x.StyleIDFlags
        FROM Times t
        INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID
        INNER JOIN Maps m ON m.ID = mc.MapID
        INNER JOIN (
            SELECT MIN(t.RunTime) AS RunTime, t.MapCourseID, t.ModeID, t.StyleIDFlags
                FROM Times t
                WHERE t.Teleports=0
                GROUP BY t.MapCourseID, t.ModeID, t.StyleIDFlags
        ) x ON x.RunTime = t.RunTime AND x.MapCourseID = t.MapCourseID AND x.ModeID = t.ModeID AND x.StyleIDFlags = t.StyleIDFlags
        WHERE m.Name = '%s' AND t.Teleports = 0
)";
