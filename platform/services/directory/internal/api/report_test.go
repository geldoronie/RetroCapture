package api

// Tests for POST /streams/{id}/report — moderation flag. No auth.
// Always returns 202; the operator pulls reports out of band.

import (
	"net/http"
	"strings"
	"testing"
)

func TestReport_HappyPath(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)

	rec, _ := doJSON(t, h, "POST", "/streams/"+id+"/report",
		ReportRequest{Reason: "spam"})
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", rec.Code)
	}
}

func TestReport_WithContact(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)

	rec, _ := doJSON(t, h, "POST", "/streams/"+id+"/report", ReportRequest{
		Reason:          "inappropriate name",
		ReporterContact: "moderator@example",
	})
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", rec.Code)
	}
}

// Empty reason is allowed (spec doesn't require a non-empty reason).
// Useful for "just flag this, see logs for context" workflows.
func TestReport_EmptyReasonOK(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	rec, _ := doJSON(t, h, "POST", "/streams/"+id+"/report", ReportRequest{})
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", rec.Code)
	}
}

func TestReport_RejectsTooLongReason(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	rec, env := doJSON(t, h, "POST", "/streams/"+id+"/report",
		ReportRequest{Reason: strings.Repeat("x", 1001)})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestReport_RejectsTooLongContact(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	rec, env := doJSON(t, h, "POST", "/streams/"+id+"/report",
		ReportRequest{ReporterContact: strings.Repeat("x", 201)})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

// Reporting a stream that doesn't exist is intentionally accepted —
// the directory does not gate reports on existence, since a report
// against a fast-expired entry should still reach the operator's
// queue. Append-only by design.
func TestReport_AgainstNonexistentStillAccepted(t *testing.T) {
	h, _ := newTestServer(t)
	rec, _ := doJSON(t, h, "POST",
		"/streams/00000000-0000-0000-0000-000000000000/report",
		ReportRequest{Reason: "test"})
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", rec.Code)
	}
}
