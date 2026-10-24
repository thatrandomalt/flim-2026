// Resource IDs (launcher dialogs, engine splash).
#define IDD_SETUP        101
#define IDC_FLDIR        1001
#define IDC_BROWSE       1002
#define IDC_NAME         1003
#define IDC_FLINFO       1004

// Launcher main window: join or host
#define IDD_MAIN         102
#define IDC_M_NAME       1101
#define IDC_M_FLINFO     1102
#define IDC_M_CHANGEFL   1103
#define IDC_M_OPENFL     1104
#define IDC_M_BACKUP     1105      // project backups on / off
#define IDC_M_BACKUPMIN  1106      //   every N minutes
#define IDC_M_PROJECTS   1107      // open the projects folder (song folders, their backups)
#define IDC_M_CRASHES    1108      // open the crash reports folder
#define IDC_M_PROJDIR    1109      // pick another projects folder
#define IDC_M_SHARED     1116      // open the shared folder
#define IDC_M_SONG       1115      // song name for an untitled project (optional)
#define IDC_M_AUTOATTACH 1117      // attach to a running FL by itself / let me pick the FL process
#define IDD_PICKFL       103       // pick the FL process (auto-attach off)
#define IDC_P_LIST       1140
#define IDC_J_URL        1110
#define IDC_J_PW         1111
#define IDC_J_REMEMBER   1112
#define IDC_J_JOIN       1113
#define IDC_H_PORT       1120
#define IDC_H_PW         1121
#define IDC_H_UPNP       1122
#define IDC_H_ALSOFL     1123
#define IDC_H_START      1124
#define IDC_H_STATUS     1125
#define IDC_H_SHARE      1126
#define IDC_H_COPY       1127
#define IDC_H_PEOPLE     1128
#define IDC_S_STATUS     1130      // session: the status FL reports (connection, people, transfers)
#define IDC_S_LOG        1131      //          the log (FL's and the host's)
#define IDC_S_LOCK       1132      //          seconds a plugin stays ours after our last change
#define IDC_S_DETAIL     1133      //          the detailed log on / off
#define IDC_S_EXPORT     1134      //          save the logs as a zip to send
#define IDC_S_CURSORS    1135      //          the others' mouse arrows shown / not
#define IDC_S_RESYNC     1136      //          load the session leader's project again (asks first)

// Engine: flim's startup splash (shown instead of FL's Title.png, only in flim's FL)
#define IDR_SPLASH       301
