from flask import Flask, jsonify, render_template, request
import requests as req
import os

app = Flask(__name__)

LEADMOBILE = "http://5.161.104.243:5000"
CREDS = {"username": "admin", "password": "admin123"}

session = req.Session()

# ── Notification config ───────────────────────────────────────────────────────
# Optional secrets live in folly.conf (gitignored) next to this file:
#   RESEND_API_KEY=re_xxx          # same Resend account the parent app uses
#   NOTIFY_EMAIL=patrick.egan@royallepage.ca
#   RESEND_FROM=noreply@patrickegan.com
# Without RESEND_API_KEY, completion still works and the email is cleanly skipped.
def _load_conf():
    conf = {
        "RESEND_API_KEY": os.environ.get("RESEND_API_KEY", ""),
        "NOTIFY_EMAIL":   "patrick.egan@royallepage.ca",
        "RESEND_FROM":    "noreply@patrickegan.com",
    }
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "folly.conf")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, _, v = line.partition("=")
                    conf[k.strip()] = v.strip()
    return conf


def auth():
    """Login and raise on failure. Re-auths if session has expired."""
    r = session.post(f"{LEADMOBILE}/api/auth/login", json=CREDS, timeout=10)
    if not r.json().get("success"):
        raise RuntimeError("auth failed")

def api_get(path, **kwargs):
    """GET from leadmobile, re-authing once on 401."""
    r = session.get(f"{LEADMOBILE}{path}", timeout=15, **kwargs)
    if r.status_code == 401:
        auth()
        r = session.get(f"{LEADMOBILE}{path}", timeout=15, **kwargs)
    r.raise_for_status()
    return r

def api_patch(path, data):
    """PATCH leadmobile, re-authing once on 401. Returns the requests.Response."""
    r = session.patch(f"{LEADMOBILE}{path}", json=data, timeout=10)
    if r.status_code == 401:
        auth()
        r = session.patch(f"{LEADMOBILE}{path}", json=data, timeout=10)
    return r

def proxy_get(path):
    try:
        return jsonify(api_get(path).json())
    except Exception as e:
        return jsonify({"error": str(e)}), 502

def proxy_patch(path, data):
    try:
        r = api_patch(path, data)
        return jsonify(r.json()), r.status_code
    except Exception as e:
        return jsonify({"error": str(e)}), 502


# ── Completion email (via Resend — same account/sender as the parent app) ──────
def _completion_email_html(lead):
    pcolor = {"A": "#dc2626", "B": "#d97706", "C": "#059669"}.get(lead.get("priority"), "#6b7280")
    def row(label, value):
        if not value:
            return ""
        return (
            '<div style="display:flex;align-items:flex-start;margin-bottom:10px;">'
            f'<span style="font-weight:500;color:#6b7280;width:110px;display:inline-block;">{label}</span>'
            f'<span style="color:#1f2937;">{value}</span></div>'
        )
    name = lead.get("name") or "Unnamed lead"
    return f"""
    <!DOCTYPE html><html><head><meta charset="utf-8"></head>
    <body style="font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;line-height:1.6;color:#374151;background:#f9fafb;margin:0;padding:20px;">
      <div style="max-width:600px;margin:0 auto;background:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1);overflow:hidden;">
        <div style="background:linear-gradient(135deg,#059669 0%,#047857 100%);color:#fff;padding:24px;text-align:center;">
          <h1 style="margin:0;font-size:24px;font-weight:600;">✅ Lead Completed</h1>
          <p style="margin:8px 0 0 0;font-size:14px;opacity:.9;">{name}</p>
        </div>
        <div style="padding:32px;">
          <div style="text-align:center;margin-bottom:24px;">
            <span style="display:inline-block;background:{pcolor};color:#fff;padding:8px 16px;border-radius:20px;font-weight:600;font-size:14px;">
              Priority {lead.get('priority') or '—'}
            </span>
          </div>
          <div style="background:#f8fafc;border-radius:8px;padding:24px;margin-bottom:24px;">
            <h2 style="margin:0 0 16px 0;color:#1f2937;font-size:20px;font-weight:600;">{name}</h2>
            {row("Type:", lead.get("type"))}
            {row("Agent:", lead.get("agent"))}
            {row("Company:", lead.get("company"))}
            {row("Phone:", lead.get("phone"))}
            {row("Last next step:", lead.get("nextStep"))}
          </div>
          <div style="background:#f0fdf4;border:1px solid #dcfce7;border-radius:8px;padding:16px;text-align:center;">
            <p style="margin:0;color:#166534;font-size:14px;">
              📋 <strong>Closed out in CXM.</strong> Follow up if anything else is needed.
            </p>
          </div>
        </div>
        <div style="background:#f8fafc;padding:16px;text-align:center;border-top:1px solid #e5e7eb;">
          <p style="margin:0;font-size:12px;color:#6b7280;">Lead Management System | Patrick Egan Real Estate Team</p>
        </div>
      </div>
    </body></html>
    """

def send_completion_email(lead):
    """Email Patrick that a lead was completed. Returns (sent, message)."""
    conf = _load_conf()
    key = conf.get("RESEND_API_KEY", "").strip()
    if not key:
        return False, "no RESEND_API_KEY configured — email skipped"
    try:
        r = req.post(
            "https://api.resend.com/emails",
            headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
            json={
                "from": conf["RESEND_FROM"],
                "to": [conf["NOTIFY_EMAIL"]],
                "subject": f"✅ Lead completed: {lead.get('name') or 'Unnamed lead'}",
                "html": _completion_email_html(lead),
            },
            timeout=15,
        )
        if r.status_code in (200, 201):
            return True, "email sent"
        return False, f"resend {r.status_code}: {r.text[:160]}"
    except Exception as e:
        return False, str(e)


@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/leads")
def leads():
    try:
        data = api_get("/api/leads").json()
        active = [l for l in data if not l.get("archived")]
        return jsonify(active)
    except Exception as e:
        return jsonify({"error": str(e)}), 502

@app.route("/api/leads/<lead_id>/tasks")
def lead_tasks(lead_id):
    return proxy_get(f"/api/leads/{lead_id}/tasks")

@app.route("/api/leads/<lead_id>", methods=["PATCH"])
def update_lead(lead_id):
    return proxy_patch(f"/api/leads/{lead_id}", request.json)

@app.route("/api/leads/<lead_id>/tasks/<task_id>", methods=["PATCH"])
def update_task(lead_id, task_id):
    return proxy_patch(f"/api/leads/{lead_id}/tasks/{task_id}", request.json)

@app.route("/api/leads/<lead_id>/archive", methods=["POST"])
def archive_lead(lead_id):
    """Archive a lead with a reason. No email — used for the Archive button."""
    reason = (request.json or {}).get("reason", "other")
    try:
        r = api_patch(f"/api/leads/{lead_id}/archive", {"reason": reason})
        return jsonify(r.json()), r.status_code
    except Exception as e:
        return jsonify({"error": str(e)}), 502

@app.route("/api/leads/<lead_id>/complete", methods=["POST"])
def complete_lead(lead_id):
    """Mark a lead completed: archive (reason=sold) and email Patrick."""
    try:
        r = api_patch(f"/api/leads/{lead_id}/archive", {"reason": "sold"})
        if r.status_code not in (200, 201):
            return jsonify({"error": "archive failed", "detail": r.text[:160]}), 502
        lead = r.json()
        sent, msg = send_completion_email(lead)
        return jsonify({"lead": lead, "emailSent": sent, "emailMessage": msg})
    except Exception as e:
        return jsonify({"error": str(e)}), 502

@app.route("/api/all-tasks")
def all_tasks():
    try:
        tasks = api_get("/api/unified-tasks", params={"limit": 200}).json()
        if not isinstance(tasks, list):
            tasks = []
        # Normalize field names to match what the native app expects
        normalized = []
        for t in tasks:
            normalized.append({
                "id":         str(t.get("id", "")),
                "title":      t.get("title", ""),
                "description": t.get("description", ""),
                "status":     t.get("status", "active"),
                "dueDate":    t.get("due_date", ""),
                "source":     t.get("source", ""),
                "task_type":  t.get("task_type", ""),
                "leadName":   t.get("lead_name", "") or "",
                "leadAgent":  t.get("lead_agent", "") or "",
                "leadPriority": t.get("lead_priority", "") or "",
                "createdAt":  t.get("created_at", ""),
                "tags":       t.get("tags", "") or "",
            })
        normalized.sort(key=lambda t: (
            t.get("status") == "completed",
            t.get("dueDate") or "9999",
            t.get("createdAt") or ""
        ))
        return jsonify(normalized)
    except Exception as e:
        return jsonify({"error": str(e)}), 502

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080)
