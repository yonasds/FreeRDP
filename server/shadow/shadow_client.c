/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/synch.h>
#include <winpr/thread.h>

#include <winpr/tools/makecert.h>

#include "shadow.h"

void shadow_client_context_new(freerdp_peer* peer, rdpShadowClient* client)
{
	rdpShadowServer* server;

	server = (rdpShadowServer*) peer->ContextExtra;
	client->server = server;

	client->StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

void shadow_client_context_free(freerdp_peer* peer, rdpShadowClient* client)
{
	CloseHandle(client->StopEvent);
}

BOOL shadow_client_capabilities(freerdp_peer* peer)
{
	return TRUE;
}

BOOL shadow_client_post_connect(freerdp_peer* peer)
{
	rdpShadowClient* client;

	client = (rdpShadowClient*) peer->context;

	fprintf(stderr, "Client from %s is activated\n", peer->hostname);

	peer->settings->DesktopWidth = client->server->screen->width;
	peer->settings->DesktopHeight = client->server->screen->height;
	peer->settings->ColorDepth = 32;

	peer->update->DesktopResize(peer->update->context);

	return TRUE;
}

BOOL shadow_client_activate(freerdp_peer* peer)
{
	rdpShadowClient* client;

	client = (rdpShadowClient*) peer->context;

	client->activated = TRUE;

	return TRUE;
}

int shadow_client_send_surface_bits(rdpShadowClient* client)
{
	int i;
	wStream* s;
	int nXSrc;
	int nYSrc;
	int nWidth;
	int nHeight;
	int nSrcStep;
	BYTE* pSrcData;
	int numMessages;
	rdpUpdate* update;
	rdpContext* context;
	rdpSettings* settings;
	rdpShadowServer* server;
	rdpShadowSurface* surface;
	rdpShadowEncoder* encoder;
	RECTANGLE_16 surfaceRect;
	SURFACE_BITS_COMMAND cmd;
	const RECTANGLE_16* extents;

	context = (rdpContext*) client;
	update = context->update;
	settings = context->settings;

	server = client->server;
	encoder = server->encoder;
	surface = server->surface;

	surfaceRect.left = 0;
	surfaceRect.top = 0;
	surfaceRect.right = surface->width;
	surfaceRect.bottom = surface->height;

	region16_intersect_rect(&(surface->invalidRegion), &(surface->invalidRegion), &surfaceRect);

	if (region16_is_empty(&(surface->invalidRegion)))
		return 1;

	extents = region16_extents(&(surface->invalidRegion));

	nXSrc = extents->left;
	nYSrc = extents->top;
	nWidth = extents->right - extents->left;
	nHeight = extents->bottom - extents->top;
	pSrcData = surface->data;
	nSrcStep = surface->scanline;

	if (settings->RemoteFxCodec)
	{
		RFX_RECT rect;
		RFX_MESSAGE* messages;

		s = encoder->rfx_s;

		rect.x = nXSrc;
		rect.y = nYSrc;
		rect.width = nWidth;
		rect.height = nHeight;

		messages = rfx_encode_messages(encoder->rfx, &rect, 1, pSrcData,
				surface->width, surface->height, nSrcStep, &numMessages,
				settings->MultifragMaxRequestSize);

		cmd.codecID = settings->RemoteFxCodecId;

		cmd.destLeft = 0;
		cmd.destTop = 0;
		cmd.destRight = surface->width;
		cmd.destBottom = surface->height;

		cmd.bpp = 32;
		cmd.width = surface->width;
		cmd.height = surface->height;

		for (i = 0; i < numMessages; i++)
		{
			Stream_SetPosition(s, 0);
			rfx_write_message(encoder->rfx, s, &messages[i]);
			rfx_message_free(encoder->rfx, &messages[i]);

			cmd.bitmapDataLength = Stream_GetPosition(s);
			cmd.bitmapData = Stream_Buffer(s);

			IFCALL(update->SurfaceBits, update->context, &cmd);
		}

		free(messages);
	}
	else if (settings->NSCodec)
	{
		NSC_MESSAGE* messages;

		s = encoder->nsc_s;

		messages = nsc_encode_messages(encoder->nsc, pSrcData,
				nXSrc, nYSrc, nWidth, nHeight, nSrcStep,
				&numMessages, settings->MultifragMaxRequestSize);

		cmd.bpp = 32;
		cmd.codecID = settings->NSCodecId;

		for (i = 0; i < numMessages; i++)
		{
			Stream_SetPosition(s, 0);

			nsc_write_message(encoder->nsc, s, &messages[i]);
			nsc_message_free(encoder->nsc, &messages[i]);

			cmd.destLeft = messages[i].x;
			cmd.destTop = messages[i].y;
			cmd.destRight = messages[i].x + messages[i].width;
			cmd.destBottom = messages[i].y + messages[i].height;
			cmd.width = messages[i].width;
			cmd.height = messages[i].height;

			cmd.bitmapDataLength = Stream_GetPosition(s);
			cmd.bitmapData = Stream_Buffer(s);

			IFCALL(update->SurfaceBits, update->context, &cmd);
		}

		free(messages);
	}

	region16_clear(&(surface->invalidRegion));

	return 0;
}

static const char* makecert_argv[4] =
{
	"makecert",
	"-rdp",
	"-live",
	"-silent"
};

static int makecert_argc = (sizeof(makecert_argv) / sizeof(char*));

int shadow_generate_certificate(rdpSettings* settings)
{
	char* serverFilePath;
	MAKECERT_CONTEXT* context;

	serverFilePath = GetCombinedPath(settings->ConfigPath, "server");

	if (!PathFileExistsA(serverFilePath))
		CreateDirectoryA(serverFilePath, 0);

	settings->CertificateFile = GetCombinedPath(serverFilePath, "server.crt");
	settings->PrivateKeyFile = GetCombinedPath(serverFilePath, "server.key");

	if ((!PathFileExistsA(settings->CertificateFile)) ||
			(!PathFileExistsA(settings->PrivateKeyFile)))
	{
		context = makecert_context_new();

		makecert_context_process(context, makecert_argc, (char**) makecert_argv);

		makecert_context_set_output_file_name(context, "server");

		if (!PathFileExistsA(settings->CertificateFile))
			makecert_context_output_certificate_file(context, serverFilePath);

		if (!PathFileExistsA(settings->PrivateKeyFile))
			makecert_context_output_private_key_file(context, serverFilePath);

		makecert_context_free(context);
	}

	free(serverFilePath);

	return 0;
}

void* shadow_client_thread(rdpShadowClient* client)
{
	DWORD status;
	DWORD nCount;
	HANDLE events[32];
	HANDLE StopEvent;
	HANDLE ClientEvent;
	HANDLE SubsystemEvent;
	freerdp_peer* peer;
	rdpSettings* settings;
	rdpShadowServer* server;
	rdpShadowSurface* surface;
	rdpShadowSubsystem* subsystem;

	server = client->server;
	surface = server->surface;
	subsystem = server->subsystem;

	peer = ((rdpContext*) client)->peer;
	settings = peer->settings;

	shadow_generate_certificate(settings);

	settings->RemoteFxCodec = TRUE;
	settings->ColorDepth = 32;

	settings->NlaSecurity = FALSE;
	settings->TlsSecurity = TRUE;
	settings->RdpSecurity = FALSE;

	peer->Capabilities = shadow_client_capabilities;
	peer->PostConnect = shadow_client_post_connect;
	peer->Activate = shadow_client_activate;

	shadow_input_register_callbacks(peer->input);

	peer->Initialize(peer);

	StopEvent = client->StopEvent;
	ClientEvent = peer->GetEventHandle(peer);
	SubsystemEvent = subsystem->event;

	while (1)
	{
		nCount = 0;
		events[nCount++] = StopEvent;
		events[nCount++] = ClientEvent;
		events[nCount++] = SubsystemEvent;

		status = WaitForMultipleObjects(nCount, events, FALSE, 250);

		if (WaitForSingleObject(client->StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		if (WaitForSingleObject(ClientEvent, 0) == WAIT_OBJECT_0)
		{
			if (!peer->CheckFileDescriptor(peer))
			{
				fprintf(stderr, "Failed to check FreeRDP file descriptor\n");
				break;
			}
		}

		if (WaitForSingleObject(SubsystemEvent, 0) == WAIT_OBJECT_0)
		{
			x11_shadow_check_event((x11ShadowSubsystem*) subsystem);
		}

		if (client->activated)
		{
			x11_shadow_surface_copy((x11ShadowSubsystem*) subsystem);
			shadow_client_send_surface_bits(client);
			region16_clear(&(surface->invalidRegion));
		}
	}

	peer->Disconnect(peer);
	
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);

	ExitThread(0);

	return NULL;
}

void shadow_client_accepted(freerdp_listener* listener, freerdp_peer* peer)
{
	rdpShadowClient* client;
	rdpShadowServer* server;

	server = (rdpShadowServer*) listener->info;

	peer->ContextExtra = (void*) server;
	peer->ContextSize = sizeof(rdpShadowClient);
	peer->ContextNew = (psPeerContextNew) shadow_client_context_new;
	peer->ContextFree = (psPeerContextFree) shadow_client_context_free;
	freerdp_peer_context_new(peer);

	client = (rdpShadowClient*) peer->context;

	client->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
			shadow_client_thread, client, 0, NULL);
}
