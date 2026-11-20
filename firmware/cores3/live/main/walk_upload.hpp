#pragma once

void walkUploadBegin();
void walkUploadService();
bool walkUploadPending();
bool queueWalkUpload(unsigned start, unsigned end);
const char* walkUploadStatus();
bool walkUploadCommand(const char* command);
