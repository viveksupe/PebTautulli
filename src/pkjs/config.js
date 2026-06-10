module.exports = [
  {
    "type": "heading",
    "defaultValue": "Tautulli Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "input",
        "messageKey": "TautulliHost",
        "label": "Host",
        "description": "e.g. https://tautulli.server.com — HTTPS requires a valid (non-self-signed) certificate",
        "defaultValue": "https://tautulli.server.com"
      },
      {
        "type": "input",
        "messageKey": "TautulliPort",
        "label": "Port",
        "description": "Leave empty for standard ports (80/443)",
        "defaultValue": "8181"
      },
      {
        "type": "input",
        "messageKey": "TautulliPath",
        "label": "URL Path Prefix",
        "description": "No leading or trailing slash. e.g. tautulli → https://server.com/tautulli/api/v2. Leave empty if Tautulli is at root.",
        "defaultValue": ""
      },
      {
        "type": "input",
        "messageKey": "TautulliApiKey",
        "label": "API Key",
        "description": "32-character key from Tautulli settings",
        "defaultValue": ""
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
