from flask import Flask, jsonify, render_template, request
import requests as req
import os

app = Flask(__name__)

LEADMOBILE = "http://5.161.104.243:5000"
CREDS = {"username": "admin", "password": "admin123"}

session = req.Session()

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

def proxy_get(path):
    try:
        return jsonify(api_get(path).json())
    except Exception as e:
        return jsonify({"error": str(e)}), 502

def proxy_patch(path, data):
    try:
        r = session.patch(f"{LEADMOBILE}{path}", json=data, timeout=10)
        if r.status_code == 401:
            auth()
            r = session.patch(f"{LEADMOBILE}{path}", json=data, timeout=10)
        return jsonify(r.json()), r.status_code
    except Exception as e:
        return jsonify({"error": str(e)}), 502

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
