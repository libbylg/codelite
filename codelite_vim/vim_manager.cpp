#include "vim_manager.h"
#include "clStatusBar.h"
#include <wx/xrc/xmlres.h>

/*EXPERIMENT*/
#include "search_thread.h"
#include <wx/tokenzr.h>
#include <wx/fontmap.h>
#include "event_notifier.h"
#include "dirpicker.h"
#include "macros.h"
#include "windowattrmanager.h"
#include <algorithm>
#include "clWorkspaceManager.h"

/**
 * Default constructor
 */

VimManager::VimManager(IManager* manager, VimSettings& settings)
    : m_settings(settings)
    , m_currentCommand()
    , m_lastCommand()
    , m_tmpBuf()
{

    m_ctrl = NULL;
    m_editor = NULL;
    m_mgr = manager;
    m_caretInsertStyle = 1;
    m_caretBlockStyle = 2;

    EventNotifier::Get()->Bind(wxEVT_ACTIVE_EDITOR_CHANGED, &VimManager::OnEditorChanged, this);
    EventNotifier::Get()->Bind(wxEVT_EDITOR_CLOSING, &VimManager::OnEditorClosing, this);
    EventNotifier::Get()->Bind(wxEVT_WORKSPACE_CLOSING, &VimManager::OnWorkspaceClosing, this);
    EventNotifier::Get()->Bind(wxEVT_ALL_EDITORS_CLOSING, &VimManager::OnAllEditorsClosing, this);
}

/**
 * Default distructor
 */

VimManager::~VimManager()
{
    EventNotifier::Get()->Unbind(wxEVT_ACTIVE_EDITOR_CHANGED, &VimManager::OnEditorChanged, this);
    EventNotifier::Get()->Unbind(wxEVT_EDITOR_CLOSING, &VimManager::OnEditorClosing, this);
    EventNotifier::Get()->Unbind(wxEVT_WORKSPACE_CLOSING, &VimManager::OnWorkspaceClosing, this);
    EventNotifier::Get()->Unbind(wxEVT_ALL_EDITORS_CLOSING, &VimManager::OnAllEditorsClosing, this);
}

/**
 * Method used to see the change of focus of the text editor:
 *
 */
void VimManager::OnEditorChanged(wxCommandEvent& event)
{
    event.Skip(); // Always call Skip() so other plugins/core components will get this event
    if(!m_settings.IsEnabled()) return;
    IEditor* editor = reinterpret_cast<IEditor*>(event.GetClientData());
    DoBindEditor(editor);
}

void VimManager::OnKeyDown(wxKeyEvent& event)
{

    wxChar ch = event.GetUnicodeKey();
    bool skip_event = true;

    if(m_ctrl == NULL || m_editor == NULL || !m_settings.IsEnabled()) {
        event.Skip();
        return;
    }

    VimCommand::eAction action = VimCommand::kNone;
    if(ch != WXK_NONE) {

        switch(ch) {
        case WXK_BACK: {
            // Delete the last comand char, if not in command mode, return true (for skip event)
            skip_event = !(m_currentCommand.DeleteLastCommandChar());
            break;
        }
        case WXK_ESCAPE:
            if(m_currentCommand.get_current_modus() == VIM_MODI::INSERT_MODUS) {
                m_tmpBuf = m_currentCommand.getTmpBuf();
            }
            skip_event = m_currentCommand.OnEscapeDown(m_ctrl);
            break;
        case WXK_RETURN: {

            skip_event = m_currentCommand.OnReturnDown(m_editor, m_mgr, action);
            break;
        }
        default:
            if(m_currentCommand.get_current_modus() == VIM_MODI::SEARCH_MODUS) {
                m_currentCommand.set_current_word(get_current_word());
                m_currentCommand.set_current_modus(VIM_MODI::NORMAL_MODUS);
            }
            skip_event = true;
            break;
        }

    } else {
        skip_event = true;
    }

    updateView();
    event.Skip(skip_event);

    // Execute the action (this will done in the next event loop)
    switch(action) {
    case VimCommand::kClose:
        CallAfter(&VimManager::CloseCurrentEditor);
        break;
    case VimCommand::kSave:
        CallAfter(&VimManager::SaveCurrentEditor);
        break;
    case VimCommand::kSaveAndClose:
        CallAfter(&VimManager::SaveCurrentEditor);
        CallAfter(&VimManager::CloseCurrentEditor);
        break;
    }
}

wxString VimManager::get_current_word()
{
    long pos = m_ctrl->GetCurrentPos();
    long start = m_ctrl->WordStartPosition(pos, true);
    long end = m_ctrl->WordEndPosition(pos, true);
    wxString word = m_ctrl->GetTextRange(start, end);
    return word;
}

void VimManager::updateView()
{
    if(m_ctrl == NULL) return;
    switch (m_currentCommand.get_current_modus() ) {
    case VIM_MODI::NORMAL_MODUS:
        m_ctrl->SetCaretStyle(m_caretBlockStyle);
        m_mgr->GetStatusBar()->SetMessage("NORMAL");
        break;
    case VIM_MODI::COMMAND_MODUS:
        m_ctrl->SetCaretStyle(m_caretBlockStyle);
        m_mgr->GetStatusBar()->SetMessage(m_currentCommand.getTmpBuf());
        break;
    case VIM_MODI::VISUAL_MODUS:
        m_ctrl->SetCaretStyle(m_caretBlockStyle);
        m_mgr->GetStatusBar()->SetMessage("VISUAL");
        break;
    default:
        m_ctrl->SetCaretStyle(m_caretInsertStyle);
        break;
    }
}

void VimManager::OnCharEvt(wxKeyEvent& event)
{
    if(!m_settings.IsEnabled()) {
        event.Skip();
        return;
    }

    bool skip_event = true;
    int modifier_key = event.GetModifiers();
    wxChar ch = event.GetUnicodeKey();

    if(ch != WXK_NONE) {

        switch(ch) {
        case WXK_ESCAPE:
            skip_event = m_currentCommand.OnEscapeDown(m_ctrl);
            break;
        default:
            skip_event = m_currentCommand.OnNewKeyDown(ch, modifier_key);
            /*FIXME save here inser tmp buffer!*/
            break;
        }

    } else {
        skip_event = true;
    }

    if(m_currentCommand.is_cmd_complete()) {

        bool repeat_last = m_currentCommand.repeat_last_cmd();

        if(repeat_last)
            repeat_cmd();
        else
            Issue_cmd();

        if(m_currentCommand.get_current_modus() != VIM_MODI::REPLACING_MODUS) {
            if(repeat_last) {
                m_currentCommand.reset_repeat_last();
            } else if(m_currentCommand.save_current_cmd()) {
                m_lastCommand = m_currentCommand;
            }
            m_currentCommand.ResetCommand();
        }
    }

    updateView();
    event.Skip(skip_event);
}

void VimManager::Issue_cmd()
{
    if(m_ctrl == NULL) return;

    m_currentCommand.issue_cmd(m_ctrl);
}

void VimManager::repeat_cmd()
{
    if(m_ctrl == NULL) return;

    m_lastCommand.repeat_issue_cmd(m_ctrl, m_tmpBuf);
}

void VimManager::CloseCurrentEditor()
{
    CHECK_PTR_RET(m_editor);

    // Fire a close event to the main frame to execute a default close tab operation
    wxCommandEvent eventClose(wxEVT_MENU, XRCID("close_file"));
    eventClose.SetEventObject(EventNotifier::Get()->TopFrame());
    EventNotifier::Get()->TopFrame()->GetEventHandler()->AddPendingEvent(eventClose);

    DoCleanup();
}

void VimManager::SaveCurrentEditor()
{
    CHECK_PTR_RET(m_editor);

    // Save the editor
    m_editor->Save();
}

void VimManager::OnEditorClosing(wxCommandEvent& event)
{
    event.Skip();
    DoCleanup();
}

void VimManager::DoCleanup(bool unbind)
{
    if(m_ctrl && unbind) {
        m_ctrl->Unbind(wxEVT_CHAR, &VimManager::OnCharEvt, this);
        m_ctrl->Unbind(wxEVT_KEY_DOWN, &VimManager::OnKeyDown, this);
        m_ctrl->SetCaretStyle(m_caretInsertStyle);
    }
    m_editor = NULL;
    m_ctrl = NULL;
    m_mgr->GetStatusBar()->SetMessage("");
}

void VimManager::SettingsUpdated()
{
    if(m_settings.IsEnabled()) {
        DoBindEditor(m_mgr->GetActiveEditor());
    } else {
        DoCleanup();
    }
}

void VimManager::DoBindEditor(IEditor* editor)
{
    DoCleanup();
    m_editor = editor;
    CHECK_PTR_RET(m_editor);

    m_ctrl = m_editor->GetCtrl();
    m_ctrl->Bind(wxEVT_CHAR, &VimManager::OnCharEvt, this);
    m_ctrl->Bind(wxEVT_KEY_DOWN, &VimManager::OnKeyDown, this);

    CallAfter(&VimManager::updateView);
}

void VimManager::OnWorkspaceClosing(wxCommandEvent& event)
{
    event.Skip();
    DoCleanup(false);
}

void VimManager::OnAllEditorsClosing(wxCommandEvent& event)
{
    event.Skip();
    DoCleanup(false);
}
