-- 002_report_id.sql — add the user-facing receipt id to stream_reports.
--
-- Each row gains a `report_id` column populated at INSERT time with
-- the same R-XXXXXXXX string the directory hands back to the user in
-- the POST /streams/<id>/report response. Lets the maintainer pivot
-- from "user quoted protocol R-AB12CD34" directly to the original
-- report row, without having to grep the service log.
--
-- Existing rows (pre-migration) get the empty default; they
-- predate the receipt feature and their reporters never had a
-- number to quote.
ALTER TABLE stream_reports ADD COLUMN report_id TEXT NOT NULL DEFAULT '';
CREATE INDEX idx_reports_report_id ON stream_reports(report_id);
