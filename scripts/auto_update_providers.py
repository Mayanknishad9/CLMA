#!/usr/bin/env python3
"""
LLM Provider Catalog Auto-Updater

Periodically fetches the latest LLM provider info and model lists from
official API endpoints and updates config/llm_catalog.json.

Runs via cron job (every 72h by default). Also supports manual invocation:
    python3 auto_update_providers.py [--dry-run] [--force]

Designed to be safe — updates catalog in place, never deletes providers,
only adds new ones or marks deprecated ones.
"""

import json
import os
import sys
import urllib.request
import urllib.error
import ssl
import copy
from datetime import datetime, timezone

# === Paths ===
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOG_PATH = os.path.join(BASE_DIR, "config", "llm_catalog.json")
API_CONFIG_PATH = os.path.join(BASE_DIR, "config", "api_config.json")

# === HTTP Helper ===
ssl_ctx = ssl.create_default_context()
ssl_ctx.check_hostname = False
ssl_ctx.verify_mode = ssl.CERT_NONE


def http_get(url, timeout=30):
    """Simple HTTP GET with urllib (no external deps)."""
    req = urllib.request.Request(url, headers={
        "User-Agent": "CLMA-Provider-Updater/1.0",
        "Accept": "application/json",
    })
    with urllib.request.urlopen(req, context=ssl_ctx, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# === Parsers ===

def parse_openai_models(data, tag_prefix=""):
    """Parse OpenAI-compatible /v1/models endpoint."""
    models = []
    for m in data.get("data", []):
        model_id = m.get("id", "")
        if not model_id:
            continue
        # Filter out non-chat models
        if any(x in model_id for x in ["embedding", "whisper", "tts", "davinci", "curation", "babbage"]):
            continue
        models.append({
            "id": f"{tag_prefix}{model_id}" if tag_prefix else model_id,
            "name": model_id.replace("-", " ").title(),
            "description": model_id,
            "max_tokens": 8192,
            "pricing": "unknown",
        })
    return models


def parse_openrouter_models(data):
    """Parse OpenRouter /api/v1/models endpoint."""
    models = []
    for m in data.get("data", []):
        model_id = m.get("id", "")
        if not model_id:
            continue
        pricing = m.get("pricing", {})
        models.append({
            "id": model_id,
            "name": m.get("name", model_id),
            "description": m.get("description", model_id)[:100],
            "max_tokens": m.get("context_length", 8192),
            "pricing": f"input: ${pricing.get('prompt', '?')}/M, output: ${pricing.get('completion', '?')}/M",
        })
    return models


def parse_together_models(data):
    """Parse Together AI /v1/models endpoint."""
    models = []
    for m in data:
        model_id = m.get("id", "")
        if not model_id:
            continue
        models.append({
            "id": model_id,
            "name": m.get("display_name", model_id),
            "description": m.get("description", model_id)[:100],
            "max_tokens": min(m.get("max_tokens", 8192), m.get("context_length", 8192)),
            "pricing": "unknown",
        })
    return models


# === Provider-to-parser mapping ===
PARSERS = {
    "openai": parse_openai_models,
    "openrouter": parse_openrouter_models,
    "together": parse_together_models,
}


def load_catalog():
    """Load current catalog from disk."""
    if os.path.exists(CATALOG_PATH):
        with open(CATALOG_PATH, "r") as f:
            return json.load(f)
    return None


def save_catalog(catalog):
    """Save catalog to disk."""
    os.makedirs(os.path.dirname(CATALOG_PATH), exist_ok=True)
    with open(CATALOG_PATH, "w") as f:
        json.dump(catalog, f, indent=2)
    print(f"✅ Catalog saved: {CATALOG_PATH}")


def update_from_source(catalog, source):
    """Try to fetch and parse models from a single update source."""
    name = source.get("name", "unknown")
    url = source.get("url", "")
    parser_name = source.get("parser", "openai")
    enabled = source.get("enabled", False)

    if not url or not enabled:
        return False, f"Disabled: {name}"

    parser = PARSERS.get(parser_name)
    if parser is None:
        return False, f"No parser for: {parser_name}"

    try:
        data = http_get(url)
        models = parser(data)
        if not models:
            return False, f"Fetched 0 models from {name}"

        # Find which provider in the catalog matches this source
        # Heuristic: match by name prefix in url
        provider_key = None
        for key, p in catalog.get("providers", {}).items():
            if key in name.lower() or key in url.lower():
                provider_key = key
                break

        if provider_key:
            old_count = len(catalog["providers"][provider_key].get("models", []))
            catalog["providers"][provider_key]["models"] = models
            new_count = len(models)
            catalog["providers"][provider_key]["last_auto_updated"] = (
                datetime.now(timezone.utc).isoformat()
            )
            return True, f"{name}: {old_count} → {new_count} models updated"
        else:
            return False, f"Fetched {len(models)} models from {name}, but no matching provider found in catalog"

    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code} fetching {name}: {e.reason}"
    except urllib.error.URLError as e:
        return False, f"Connection failed for {name}: {e.reason}"
    except json.JSONDecodeError as e:
        return False, f"Bad JSON from {name}: {e}"
    except Exception as e:
        return False, f"Error updating {name}: {e}"


def update_catalog(catalog):
    """Run all enabled update sources against the catalog."""
    results = []
    for source in catalog.get("update_sources", []):
        success, msg = update_from_source(catalog, source)
        results.append({"source": source.get("name"), "success": success, "message": msg})
        print(f"  {'✅' if success else '❌'} {msg}")
    return results


def auto_fix_api_config(catalog):
    """Ensure api_config.json provider/model still exists in catalog."""
    if not os.path.exists(API_CONFIG_PATH):
        return
    with open(API_CONFIG_PATH, "r") as f:
        config = json.load(f)

    provider = config.get("provider")
    model = config.get("model")

    if provider and provider not in catalog.get("providers", {}):
        print(f"⚠️  Provider '{provider}' not in catalog anymore. Resetting to first available...")
        first = next(iter(catalog["providers"]))
        config["provider"] = first
        config["model"] = catalog["providers"][first]["models"][0]["id"]
        config["base_url"] = catalog["providers"][first].get("default_base_url", "")
        with open(API_CONFIG_PATH, "w") as f:
            json.dump(config, f, indent=2)
        print(f"  → Reset to {first} / {config['model']}")


def main():
    dry_run = "--dry-run" in sys.argv
    force = "--force" in sys.argv

    catalog = load_catalog()
    if catalog is None:
        print("❌ No catalog found at:", CATALOG_PATH)
        sys.exit(1)

    print(f"📦 LLM Provider Catalog Updater v{catalog.get('version', '?')}")
    print(f"   Last updated: {catalog.get('last_updated', 'never')}")
    print(f"   Auto-update: {'enabled' if catalog.get('auto_update_enabled', True) else 'disabled'}")
    print()

    if not force:
        # Check if enough time has passed
        last = catalog.get("last_updated", "")
        if last:
            try:
                last_dt = datetime.fromisoformat(last)
                hours_since = (datetime.now(timezone.utc) - last_dt).total_seconds() / 3600
                interval = catalog.get("update_interval_hours", 72)
                if hours_since < interval:
                    remaining = int(interval - hours_since)
                    print(f"⏳ Only {hours_since:.1f}h since last update (interval: {interval}h).")
                    print(f"   Use --force to update anyway.")
                    if not dry_run:
                        print("   Skipping.")
                        return
            except (ValueError, TypeError):
                pass

    print("🔄 Fetching latest provider data...\n")
    results = update_catalog(catalog)

    success_count = sum(1 for r in results if r["success"])
    print(f"\n📊 Results: {success_count}/{len(results)} sources updated successfully")

    if not dry_run:
        catalog["last_updated"] = datetime.now(timezone.utc).isoformat()
        save_catalog(catalog)
        auto_fix_api_config(catalog)
    else:
        print(f"\n🔍 DRY RUN — catalog not saved")

    # Generate update summary
    outdated = [r for r in results if not r["success"]]
    if outdated:
        print(f"\n⚠️  Failed sources ({len(outdated)}):")
        for r in outdated:
            print(f"   • {r['source']}: {r['message']}")


if __name__ == "__main__":
    main()
