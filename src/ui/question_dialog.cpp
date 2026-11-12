// A coding agent's questions with choices (Claude Code's AskUserQuestion):
// each question is a labelled group of radio buttons (one answer) or check
// boxes (any number), with a box for an answer in the user's own words.

#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/providers/provider.h"
#include "core/util.h"

#include <wx/statbox.h>

#include <algorithm>

namespace {

class QuestionsDialog : public wxDialog {
public:
    QuestionsDialog(wxWindow* parent, const wxString& asker, const std::vector<AgentQuestion>& questions)
        : wxDialog(parent, wxID_ANY, questions.size() == 1 ? wxString("Question") : wxString("Questions"), wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_questions(questions) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        wxString intro = asker + " asks you:";
        if (questions.size() > 1) intro = asker + wxString::Format(" asks you %d questions:", static_cast<int>(questions.size()));
        sizer->Add(new wxStaticText(this, wxID_ANY, intro), 0, wxALL, 8);

        auto* scroll = new wxScrolledWindow(this, wxID_ANY);
        scroll->SetScrollRate(0, 10);
        auto* list = new wxBoxSizer(wxVERTICAL);
        for (const auto& q : m_questions) {
            Group g;
            wxString title = U(q.question);
            if (!q.header.empty() && q.header != q.question) title = U(q.header) + ": " + title;
            if (q.multiSelect) title += " (choose any)";
            auto* box = new wxStaticBoxSizer(wxVERTICAL, scroll, wxControl::EscapeMnemonics(title));
            wxWindow* parentBox = box->GetStaticBox();
            for (size_t i = 0; i < q.options.size(); i++) {
                const auto& o = q.options[i];
                wxString label = U(o.label);
                if (!o.description.empty()) label += " - " + U(o.description);
                label = wxControl::EscapeMnemonics(label);
                if (q.multiSelect) {
                    auto* check = new wxCheckBox(parentBox, wxID_ANY, label);
                    g.checks.push_back(check);
                    box->Add(check, 0, wxALL, 4);
                } else {
                    auto* radio = new wxRadioButton(parentBox, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, i == 0 ? wxRB_GROUP : 0);
                    g.radios.push_back(radio);
                    box->Add(radio, 0, wxALL, 4);
                }
            }
            if (!q.multiSelect) {
                g.otherRadio = new wxRadioButton(parentBox, wxID_ANY, "Something else (write it below)", wxDefaultPosition,
                                                 wxDefaultSize, q.options.empty() ? wxRB_GROUP : 0);
                box->Add(g.otherRadio, 0, wxALL, 4);
            }
            g.other = new wxTextCtrl(parentBox, wxID_ANY);
            box->Add(LabeledRow(parentBox, q.multiSelect ? "Anything else:" : "Your own answer:", g.other), 0, wxEXPAND | wxALL, 4);
            if (g.otherRadio) {
                // Writing an answer of one's own chooses it.
                wxRadioButton* otherRadio = g.otherRadio;
                g.other->Bind(wxEVT_TEXT, [otherRadio](wxCommandEvent& e) {
                    if (!e.GetString().empty()) otherRadio->SetValue(true);
                    e.Skip();
                });
            }
            list->Add(box, 0, wxEXPAND | wxALL, 6);
            m_groups.push_back(g);
        }
        scroll->SetSizer(list);
        scroll->FitInside();
        sizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

        auto* buttons = new wxStdDialogButtonSizer();
        auto* ok = new wxButton(this, wxID_OK, "&Answer");
        auto* cancel = new wxButton(this, wxID_CANCEL, "&Don't answer");
        buttons->AddButton(ok);
        buttons->AddButton(cancel);
        buttons->Realize();
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
        SetSizer(sizer);
        ok->SetDefault();

        // As tall as the questions need, up to a limit (then it scrolls).
        wxSize content = list->GetMinSize();
        SetSize(std::max(FromDIP(560), content.x + FromDIP(40)), std::min(FromDIP(620), content.y + FromDIP(170)));
        CentreOnParent();
        Bind(wxEVT_BUTTON, &QuestionsDialog::OnAnswer, this, wxID_OK);
        // Start on the first question's first choice.
        if (!m_groups.empty()) {
            const Group& first = m_groups.front();
            if (!first.radios.empty()) first.radios.front()->SetFocus();
            else if (!first.checks.empty()) first.checks.front()->SetFocus();
            else first.other->SetFocus();
        }
    }

    const std::vector<std::string>& Answers() const { return m_answers; }

private:
    struct Group {
        std::vector<wxRadioButton*> radios;
        wxRadioButton* otherRadio = nullptr;
        std::vector<wxCheckBox*> checks;
        wxTextCtrl* other = nullptr;
    };

    void OnAnswer(wxCommandEvent& e) {
        std::vector<std::string> answers;
        for (size_t i = 0; i < m_groups.size(); i++) {
            const Group& g = m_groups[i];
            const AgentQuestion& q = m_questions[i];
            std::string own = Trim(S(g.other->GetValue()));
            if (q.multiSelect) {
                std::string chosen;
                for (size_t k = 0; k < g.checks.size(); k++) {
                    if (g.checks[k]->GetValue()) chosen += (chosen.empty() ? "" : ", ") + q.options[k].label;
                }
                if (!own.empty()) chosen += (chosen.empty() ? "" : ", ") + own;
                answers.push_back(chosen.empty() ? std::string("None of these") : chosen);
                continue;
            }
            std::string chosen;
            for (size_t k = 0; k < g.radios.size(); k++) {
                if (g.radios[k]->GetValue()) chosen = q.options[k].label;
            }
            if (g.otherRadio && g.otherRadio->GetValue()) chosen = own;
            if (chosen.empty()) {
                ShowError(this, "Choose an answer for: " + U(q.question), "Questions");
                if (g.otherRadio && g.otherRadio->GetValue()) g.other->SetFocus();
                else if (!g.radios.empty()) g.radios.front()->SetFocus();
                return;
            }
            answers.push_back(chosen);
        }
        m_answers = answers;
        e.Skip();  // closes with wxID_OK
    }

    std::vector<AgentQuestion> m_questions;
    std::vector<Group> m_groups;
    std::vector<std::string> m_answers;
};

}  // namespace

bool AskAgentQuestions(wxWindow* parent, const wxString& asker, const std::vector<AgentQuestion>& questions,
                       std::vector<std::string>& answers) {
    QuestionsDialog dlg(parent, asker, questions);
    if (dlg.ShowModal() != wxID_OK) return false;
    answers = dlg.Answers();
    return answers.size() == questions.size();
}
