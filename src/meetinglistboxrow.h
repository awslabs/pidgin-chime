#ifndef CHIME_MEETING_LIST_BOX_ROW_H
#define CHIME_MEETING_LIST_BOX_ROW_H

#include <gtk/gtk.h>

#include "chime-contact.h"
#include "chime-conversation.h"
#include "chime-room.h"
#include "chime-meeting.h"

G_BEGIN_DECLS

#define CHIME_TYPE_MEETING_LIST_BOX_ROW (chime_meeting_list_box_row_get_type ())
G_DECLARE_FINAL_TYPE(ChimeMeetingListBoxRow, chime_meeting_list_box_row, CHIME, MEETING_LIST_BOX_ROW, GtkListBoxRow)

GtkWidget       *chime_meeting_list_box_row_new            (ChimeMeeting           *meeting);

ChimeMeeting    *chime_meeting_list_box_row_get_meeting    (ChimeMeetingListBoxRow *self);

G_END_DECLS

#endif /* CHIME_MEETING_LIST_BOX_ROW_H */

/* ex:set ts=4 et: */
