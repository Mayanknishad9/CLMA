"""
LLM API Provider Abstraction Layer.

Adapter pattern: each provider normalizes its API into a common interface.
Supports OpenAI, Anthropic Claude, DeepSeek, Google Gemini, and OpenAI-compatible local endpoints.
Zero heavy dependencies — uses urllib from stdlib + built-in json.
"""
import json
import os
import urllib.request
import urllib.error
import ssl
from typing import Optional

CONFIG_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "config", "api_config.json")

# === Provider Registry ===

PROVIDER_REGISTRY = {}

def register_provider(name):
    """Decorator to register a provider class."""
    def wrapper(cls):
        PROVIDER_REGISTRY[name] = cls
        return cls
    return wrapper

def get_provider(name: str) -> type:
    """Get a provider class by name."""
    if name not in PROVIDER_REGISTRY:
        raise ValueError(f"Unknown provider: {name}. Available: {list(PROVIDER_REGISTRY.keys())}")
    return PROVIDER_REGISTRY[name]

# === Provider Defaults ===

PROVIDER_DEFAULTS = {
    "openai": {
        "model": "gpt-4o",
        "base_url": "https://api.openai.com/v1",
    },
    "anthropic": {
        "model": "claude-sonnet-4-20250514",
        "base_url": "https://api.anthropic.com/v1",
    },
    "deepseek": {
        "model": "deepseek-chat",
        "base_url": "https://api.deepseek.com/v1",
    },
    "gemini": {
        "model": "gemini-2.0-flash",
        "base_url": "https://generativelanguage.googleapis.com/v1beta",
    },
    "local": {
        "model": "llama3",
        "base_url": "http://localhost:11434/v1",
    },
}

PROVIDER_MODELS = {
    "openai": [
        "gpt-4o", "gpt-4o-mini", "gpt-4-turbo", "gpt-3.5-turbo",
        "o1", "o3-mini",
    ],
    "anthropic": [
        "claude-sonnet-4-20250514", "claude-sonnet-4-20250514",
        "claude-opus-4-20250514", "claude-3-5-sonnet-20241022",
        "claude-3-5-haiku-20241022",
    ],
    "deepseek": [
        "deepseek-chat", "deepseek-reasoner",
    ],
    "gemini": [
        "gemini-2.0-flash", "gemini-2.0-pro", "gemini-2.5-pro",
        "gemini-1.5-pro", "gemini-1.5-flash",
    ],
    "local": [
        "llama3", "llama3.1", "mistral", "mixtral", "qwen2",
        "codellama", "deepseek-coder", "phi3",
    ],
}


# === Base Provider ===

class LLMProvider:
    """Abstract base for LLM API providers."""

    def __init__(self, config: dict):
        self.api_key = config.get("api_key", "")
        self.model = config.get("model", self.default_model())
        self.base_url = config.get("base_url", self.default_base_url())
        self.temperature = float(config.get("temperature", 0.7))
        self.max_tokens = int(config.get("max_tokens", 2048))

    @classmethod
    def default_model(cls) -> str:
        return PROVIDER_DEFAULTS.get(cls.__name__.replace("Provider", "").lower(), {}).get("model", "")

    @classmethod
    def default_base_url(cls) -> str:
        return PROVIDER_DEFAULTS.get(cls.__name__.replace("Provider", "").lower(), {}).get("base_url", "")

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        """Send a chat completion and return response text."""
        raise NotImplementedError

    def test_connection(self) -> tuple[bool, str]:
        """Test API connectivity. Returns (success, message)."""
        try:
            result = self.chat([
                {"role": "system", "content": "You are a helpful assistant."},
                {"role": "user", "content": "Reply with exactly: OK"},
            ], temperature=0.1)
            if "OK" in result.strip():
                return True, f"Connection OK · Model: {self.model}"
            return True, f"Connected (unexpected response: {result.strip()[:50]})"
        except Exception as e:
            return False, str(e)

    def _http_request(self, url: str, data: dict, headers: dict) -> dict:
        """Make an HTTP POST with JSON body. Uses urllib — zero deps beyond stdlib."""
        body = json.dumps(data).encode("utf-8")
        req = urllib.request.Request(url, data=body, headers=headers, method="POST")
        # Create SSL context that doesn't verify in dev (so localhost certs work)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=120) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            error_body = e.read().decode("utf-8") if e.fp else ""
            raise RuntimeError(f"HTTP {e.code}: {e.reason}\n{error_body[:300]}")
        except urllib.error.URLError as e:
            raise RuntimeError(f"Connection failed: {e.reason}")


# === Concrete Providers ===

@register_provider("openai")
class OpenAIProvider(LLMProvider):
    """OpenAI API (also covers DeepSeek, Groq, Together, etc. via base_url override)."""

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        url = f"{self.base_url.rstrip('/')}/chat/completions"
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        data = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature if temperature is not None else self.temperature,
            "max_tokens": self.max_tokens,
        }
        result = self._http_request(url, data, headers)
        try:
            choice = result["choices"][0]
            finish_reason = choice.get("finish_reason", "")
            content = choice["message"]["content"]
            if finish_reason == "length":
                import warnings
                warnings.warn(
                    f"[{self.__class__.__name__}] Response truncated at {self.max_tokens} tokens "
                    f"(finish_reason=length). Content length: {len(content)} chars."
                )
            return content
        except (KeyError, IndexError) as e:
            raise RuntimeError(f"Unexpected API response: {json.dumps(result, indent=2)[:300]}")


@register_provider("anthropic")
class AnthropicProvider(LLMProvider):
    """Anthropic Claude API."""

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        url = f"{self.base_url.rstrip('/')}/messages"
        headers = {
            "x-api-key": self.api_key,
            "anthropic-version": "2023-06-01",
            "Content-Type": "application/json",
        }
        # Convert OpenAI-format messages to Anthropic format
        system_msg = None
        anthropic_messages = []
        for msg in messages:
            if msg["role"] == "system":
                system_msg = msg["content"]
            else:
                anthropic_messages.append({
                    "role": msg["role"],
                    "content": msg["content"],
                })
        data = {
            "model": self.model,
            "messages": anthropic_messages,
            "temperature": temperature if temperature is not None else self.temperature,
            "max_tokens": self.max_tokens,
        }
        if system_msg:
            data["system"] = system_msg
        result = self._http_request(url, data, headers)
        try:
            return result["content"][0]["text"]
        except (KeyError, IndexError) as e:
            raise RuntimeError(f"Unexpected API response: {json.dumps(result, indent=2)[:300]}")


@register_provider("deepseek")
class DeepSeekProvider(LLMProvider):
    """DeepSeek API (OpenAI-compatible, but different defaults)."""

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        # OpenAI-compatible
        url = f"{self.base_url.rstrip('/')}/chat/completions"
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        data = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature if temperature is not None else self.temperature,
            "max_tokens": self.max_tokens,
        }
        result = self._http_request(url, data, headers)
        try:
            choice = result["choices"][0]
            finish_reason = choice.get("finish_reason", "")
            content = choice["message"]["content"]
            # Warn if the response was truncated by max_tokens
            if finish_reason == "length":
                import warnings
                warnings.warn(
                    f"[{self.__class__.__name__}] Response truncated at {self.max_tokens} tokens "
                    f"(finish_reason=length). Content length: {len(content)} chars. "
                    f"Increase max_tokens in api_config.json for longer output."
                )
            return content
        except (KeyError, IndexError) as e:
            raise RuntimeError(f"Unexpected API response: {json.dumps(result, indent=2)[:300]}")


@register_provider("gemini")
class GeminiProvider(LLMProvider):
    """Google Gemini API."""

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        # Gemini uses a different endpoint structure
        url = f"{self.base_url.rstrip('/')}/models/{self.model}:generateContent?key={self.api_key}"
        headers = {"Content-Type": "application/json"}

        # Convert OpenAI format to Gemini format
        gemini_contents = []
        system_instruction = None
        for msg in messages:
            if msg["role"] == "system":
                system_instruction = msg["content"]
            else:
                role = "model" if msg["role"] == "assistant" else "user"
                gemini_contents.append({
                    "role": role,
                    "parts": [{"text": msg["content"]}],
                })

        data = {
            "contents": gemini_contents,
            "generationConfig": {
                "temperature": temperature if temperature is not None else self.temperature,
                "maxOutputTokens": self.max_tokens,
            },
        }
        if system_instruction:
            data["systemInstruction"] = {"parts": [{"text": system_instruction}]}

        result = self._http_request(url, data, headers)
        try:
            return result["candidates"][0]["content"]["parts"][0]["text"]
        except (KeyError, IndexError) as e:
            raise RuntimeError(f"Unexpected Gemini response: {json.dumps(result, indent=2)[:300]}")


@register_provider("local")
class LocalProvider(LLMProvider):
    """Local OpenAI-compatible endpoint (Ollama, vLLM, llama.cpp, etc.)."""

    def chat(self, messages: list, temperature: Optional[float] = None) -> str:
        url = f"{self.base_url.rstrip('/')}/chat/completions"
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"

        data = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature if temperature is not None else self.temperature,
            "max_tokens": self.max_tokens,
            "stream": False,
        }
        result = self._http_request(url, data, headers)
        try:
            return result["choices"][0]["message"]["content"]
        except (KeyError, IndexError) as e:
            raise RuntimeError(f"Unexpected response from local API: {json.dumps(result, indent=2)[:300]}")


# === Config Management ===

def load_config() -> dict:
    """Load API configuration from disk."""
    if not os.path.exists(CONFIG_PATH):
        return {
            "provider": "openai",
            "api_key": "",
            "model": PROVIDER_DEFAULTS["openai"]["model"],
            "base_url": PROVIDER_DEFAULTS["openai"]["base_url"],
            "temperature": 0.7,
            "max_tokens": 2048,
            "enabled": False,
        }
    with open(CONFIG_PATH, "r") as f:
        return json.load(f)


def save_config(config: dict):
    """Save API configuration to disk."""
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        json.dump(config, f, indent=2)


def create_provider(config: Optional[dict] = None) -> Optional[LLMProvider]:
    """Create a provider instance from config. Returns None if not enabled."""
    if config is None:
        config = load_config()
    if not config.get("enabled", False):
        return None
    provider_name = config.get("provider", "openai")
    provider_class = get_provider(provider_name)
    return provider_class(config)


def get_available_providers() -> list:
    """Get list of available provider names with metadata."""
    return [
        {
            "id": name,
            "name": cls.__name__.replace("Provider", ""),
            "default_model": cls.default_model(),
            "default_base_url": cls.default_base_url(),
            "models": PROVIDER_MODELS.get(name, []),
        }
        for name, cls in PROVIDER_REGISTRY.items()
    ]


# === LLM Catalog Support ===

CATALOG_PATH = os.path.join(os.path.dirname(CONFIG_PATH), "llm_catalog.json")


def load_catalog() -> dict:
    """Load the LLM provider catalog from disk. Returns empty dict on failure."""
    if not os.path.exists(CATALOG_PATH):
        return {}
    try:
        with open(CATALOG_PATH, "r") as f:
            return json.load(f)
    except (json.JSONDecodeError, IOError):
        return {}


def get_catalog_providers() -> dict:
    """Get all provider entries from the catalog."""
    catalog = load_catalog()
    return catalog.get("providers", {})


def get_catalog_models(provider_id: str) -> list:
    """Get models for a specific provider from the catalog."""
    providers = get_catalog_providers()
    p = providers.get(provider_id)
    if p is None:
        return []
    return p.get("models", [])


def get_catalog_categories() -> dict:
    """Get catalog categories."""
    catalog = load_catalog()
    return catalog.get("categories", {})


def get_catalog_api_types() -> dict:
    """Get API type definitions from catalog."""
    catalog = load_catalog()
    return catalog.get("api_types", {})
