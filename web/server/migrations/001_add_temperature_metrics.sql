ALTER TABLE logs
ADD COLUMN temperature_metrics JSON NOT NULL DEFAULT '[]'::json;
