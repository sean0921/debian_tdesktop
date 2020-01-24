/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_media.h"
#include "ui/effects/animations.h"
#include "data/data_poll.h"
#include "base/weak_ptr.h"

namespace Ui {
class RippleAnimation;
class FireworksAnimation;
} // namespace Ui

namespace HistoryView {

class Poll : public Media, public base::has_weak_ptr {
public:
	Poll(
		not_null<Element*> parent,
		not_null<PollData*> poll);

	void draw(Painter &p, const QRect &r, TextSelection selection, crl::time ms) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const override {
		return true;
	}
	bool dragItemByHandler(const ClickHandlerPtr &p) const override {
		return true;
	}

	bool needsBubble() const override {
		return true;
	}
	bool customInfoLayout() const override {
		return false;
	}

	BubbleRoll bubbleRoll() const override;
	QMargins bubbleRollRepaintMargins() const override;
	void paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const override;

	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	~Poll();

private:
	struct AnswerAnimation;
	struct AnswersAnimation;
	struct SendingAnimation;
	struct Answer;

	QSize countOptimalSize() override;
	QSize countCurrentSize(int newWidth) override;

	[[nodiscard]] bool showVotes() const;
	[[nodiscard]] bool canVote() const;
	[[nodiscard]] bool canSendVotes() const;

	[[nodiscard]] int countAnswerTop(
		const Answer &answer,
		int innerWidth) const;
	[[nodiscard]] int countAnswerHeight(
		const Answer &answer,
		int innerWidth) const;
	[[nodiscard]] ClickHandlerPtr createAnswerClickHandler(
		const Answer &answer);
	void updateTexts();
	void updateRecentVoters();
	void updateAnswers();
	void updateVotes();
	void updateTotalVotes();
	bool showVotersCount() const;
	bool inlineFooter() const;
	void updateAnswerVotes();
	void updateAnswerVotesFromOriginal(
		Answer &answer,
		const PollAnswer &original,
		int percent,
		int maxVotes);
	void checkSendingAnimation() const;

	void paintRecentVoters(
		Painter &p,
		int left,
		int top,
		TextSelection selection) const;
	int paintAnswer(
		Painter &p,
		const Answer &answer,
		const AnswerAnimation *animation,
		int left,
		int top,
		int width,
		int outerWidth,
		TextSelection selection) const;
	void paintRadio(
		Painter &p,
		const Answer &answer,
		int left,
		int top,
		TextSelection selection) const;
	void paintPercent(
		Painter &p,
		const QString &percent,
		int percentWidth,
		int left,
		int top,
		int outerWidth,
		TextSelection selection) const;
	void paintFilling(
		Painter &p,
		bool chosen,
		bool correct,
		float64 filling,
		int left,
		int top,
		int width,
		int height,
		TextSelection selection) const;
	void paintInlineFooter(
		Painter &p,
		int left,
		int top,
		int paintw,
		TextSelection selection) const;
	void paintBottom(
		Painter &p,
		int left,
		int top,
		int paintw,
		TextSelection selection) const;

	bool checkAnimationStart() const;
	bool answerVotesChanged() const;
	void saveStateInAnimation() const;
	void startAnswersAnimation() const;
	void resetAnswersAnimation() const;
	void radialAnimationCallback() const;

	void toggleRipple(Answer &answer, bool pressed);
	void toggleLinkRipple(bool pressed);
	void toggleMultiOption(const QByteArray &option);
	void sendMultiOptions();
	void showResults();
	void checkQuizAnswered();

	[[nodiscard]] int bottomButtonHeight() const;

	const not_null<PollData*> _poll;
	int _pollVersion = 0;
	int _totalVotes = 0;
	bool _voted = false;
	PollData::Flags _flags = PollData::Flags();

	Ui::Text::String _question;
	Ui::Text::String _subtitle;
	std::vector<not_null<UserData*>> _recentVoters;
	QImage _recentVotersImage;

	std::vector<Answer> _answers;
	Ui::Text::String _totalVotesLabel;
	ClickHandlerPtr _showResultsLink;
	ClickHandlerPtr _sendVotesLink;
	mutable std::unique_ptr<Ui::RippleAnimation> _linkRipple;

	mutable std::unique_ptr<AnswersAnimation> _answersAnimation;
	mutable std::unique_ptr<SendingAnimation> _sendingAnimation;
	mutable std::unique_ptr<Ui::FireworksAnimation> _fireworksAnimation;
	Ui::Animations::Simple _wrongAnswerAnimation;
	mutable QPoint _lastLinkPoint;

	bool _hasSelected = false;
	bool _votedFromHere = false;
	mutable bool _wrongAnswerAnimated = false;

};

} // namespace HistoryView
