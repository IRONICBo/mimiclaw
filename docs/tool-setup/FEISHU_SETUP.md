# Feishu / Lark Bot Configuration Guide

This guide walks through setting up a Feishu (or Lark) bot to work with MimiClaw, turning your ESP32-S3 into a Feishu-connected AI assistant.

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Step 1: Create a Feishu App](#step-1-create-a-feishu-app)
- [Step 2: Configure App Permissions](#step-2-configure-app-permissions)
- [Step 3: Set Up Event Subscription](#step-3-set-up-event-subscription)
- [Step 4: Configure MimiClaw](#step-4-configure-mimiclaw)
- [Step 5: Network Setup](#step-5-network-setup)
- [Step 6: Publish and Test](#step-6-publish-and-test)
- [Architecture](#architecture)
- [CLI Commands](#cli-commands)
- [Troubleshooting](#troubleshooting)
- [References](#references)

## Overview

MimiClaw supports Feishu as a messaging channel alongside Telegram and WebSocket. The Feishu integration uses:

- **Webhook receiver** — the ESP32 runs an HTTP server on port 18790 to receive messages from Feishu
- **Send API** — MimiClaw sends replies via Feishu's REST API (`/im/v1/messages`)
- **Tenant access token** — automatic token management with background refresh

Both **direct messages (P2P)** and **group chats** are supported.

## Prerequisites

- A Feishu account (sign up at [feishu.cn](https://www.feishu.cn)) or a Lark account ([larksuite.com](https://www.larksuite.com))
- Admin access to create apps on [Feishu Open Platform](https://open.feishu.cn/) (or [Lark Developer](https://open.larksuite.com/))
- MimiClaw flashed on an ESP32-S3 with network access
- The ESP32 must be reachable from the internet (see [Network Setup](#step-5-network-setup))

## Step 1: Create a Feishu App

1. Go to [Feishu Open Platform](https://open.feishu.cn/) and sign in
2. Click **Create Custom App** (or "Create App" on Lark)
3. Fill in the app details:
   - **App Name**: Choose a name (e.g., "MimiClaw Bot")
   - **App Description**: Brief description of your bot
   - **App Icon**: Upload an icon (optional)
4. After creation, you will see your **App ID** and **App Secret** on the app's **Credentials & Basic Info** page

> **Important:** Save the **App ID** (`cli_xxxxxxxxxxxxxx`) and **App Secret** (`xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx`). You will need these to configure MimiClaw.

## Step 2: Configure App Permissions

In your app's settings, go to **Permissions & Scopes** and add these required permissions:

| Permission | Scope ID | Description |
|-----------|----------|-------------|
| Read/Send messages | `im:message` | Receive and send messages |
| Send messages as bot | `im:message:send_as_bot` | Send messages as the bot identity |

To add permissions:

1. Navigate to **Permissions & Scopes** in the left sidebar
2. Search for each scope ID listed above
3. Click **Add** next to each permission
4. The permissions will take effect after you publish or update the app version

> **Note:** On Lark (international version), the permission names may differ slightly, but the scope IDs are the same.
