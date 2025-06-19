#!/bin/env bash

if [ "$SPOOFED" = "true" ]; then

msg="\\#ci\\_$VERSION

Synced up with $COMMIT_URL

*This manager package id has been spoofed*
"
else
msg="*$TITLE*
\\#ci\\_$VERSION
\`\`\`
$COMMIT_MESSAGE
\`\`\`
[Commit]($COMMIT_URL)
[Workflow run]($RUN_URL)
"
fi

file="$1"

curl -s -F document=@$file "https://api.telegram.org/bot$BOT_TOKEN/sendDocument" \
	-F chat_id="$CHAT_ID" \
	-F message_thread_id="$MESSAGE_THREAD_ID" \
	-F "disable_web_page_preview=true" \
	-F "parse_mode=markdownv2" \
	-F caption="$msg"
