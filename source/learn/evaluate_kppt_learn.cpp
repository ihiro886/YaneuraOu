﻿#ifndef _EVALUATE_LEARN_CPP_
#define _EVALUATE_LEARN_CPP_

// KPPT評価関数の学習時用のコード

#include "../shogi.h"

#if defined(EVAL_LEARN)

#include "learn.h"

#include "../evaluate.h"
#include "../eval/evaluate_kppt.h"
#include "../eval/kppt_evalsum.h"
#include "../position.h"
#include "../misc.h"

using namespace std;

namespace Eval
{
	// 絶対値を抑制するマクロ
#define SET_A_LIMIT_TO(X,MIN,MAX)    \
	X[0] = std::min(X[0],(MAX));     \
	X[0] = std::max(X[0],(MIN));     \
	X[1] = std::min(X[1],(MAX));     \
	X[1] = std::max(X[1],(MIN));

	typedef std::array<int32_t, 2> ValueKk;
	typedef std::array<int16_t, 2> ValueKpp;
	typedef std::array<int32_t, 2> ValueKkp;


	// 学習で用いる、手番込みの浮動小数点数。
	typedef std::array<LearnFloatType, 2> FloatPair;

	// FloatPairに対する基本的な演算。
	FloatPair operator + (FloatPair x, LearnFloatType a) { return FloatPair{ x[0] + a, x[1] + a }; }
	FloatPair operator - (FloatPair x, LearnFloatType a) { return FloatPair{ x[0] - a, x[1] - a }; }
	FloatPair operator * (FloatPair x, LearnFloatType a) { return FloatPair{ x[0] * a, x[1] * a }; }
	FloatPair operator * (LearnFloatType a , FloatPair x) { return FloatPair{ x[0] * a, x[1] * a }; }
	FloatPair operator / (FloatPair x, LearnFloatType a) { return FloatPair{ x[0] / a, x[1] / a }; }

	FloatPair operator + (FloatPair x, FloatPair y) { return FloatPair{ x[0] + y[0], x[1] + y[1] }; }
	FloatPair operator - (FloatPair x, FloatPair y) { return FloatPair{ x[0] - y[0], x[1] - y[1] }; }
	FloatPair operator * (FloatPair x, FloatPair y) { return FloatPair{ x[0] * y[0], x[1] * y[1] }; }
	FloatPair operator / (FloatPair x, FloatPair y) { return FloatPair{ x[0] / y[0], x[1] / y[1] }; }

	// あるBonaPieceを相手側から見たときの値
	BonaPiece inv_piece[fe_end];

	// 盤面上のあるBonaPieceをミラーした位置にあるものを返す。
	BonaPiece mir_piece[fe_end];

	// 学習時にkppテーブルに値を書き出すためのヘルパ関数。
	// この関数を用いると、ミラー関係にある箇所などにも同じ値を書き込んでくれる。次元下げの一種。
	void kpp_write(Square k1, BonaPiece p1, BonaPiece p2, ValueKpp value)
	{
		// '~'をinv記号,'M'をmirror記号だとして
		//   [  k1 ][  p1 ][  p2 ]
		//   [  k1 ][  p2 ][  p1 ]
		//   [M(k1)][M(p1)][M(p2)]
		//   [M(k1)][M(p2)][M(p1)]
		// は、同じ値であるべきなので、その4箇所に書き出す。

		BonaPiece mp1 = mir_piece[p1];
		BonaPiece mp2 = mir_piece[p2];
		Square  mk1 = Mir(k1);

		// kppのppを入れ替えたときのassert

#if 0
		// Apery(WCSC26)の評価関数、玉が5筋にいるきにmirrorした値が一致しないので
		// assertから除外しておく。
		ASSERT_LV3(kpp[k1][p1][p2] == kpp[mk1][mp1][mp2] || file_of(k1) == FILE_5);
		ASSERT_LV3(kpp[k1][p1][p2] == kpp[mk1][mp2][mp1] || file_of(k1) == FILE_5);
#endif

		// このassert書いておきたいが、many core(e.g.HT40)だと
		// 別のスレッドから同時に２つに異なる値を書き込むことがあるのでassertで落ちてしまう。
//		ASSERT_LV3(kpp[k1][p1][p2] == kpp[k1][p2][p1]);

		kpp[k1][p1][p2]
			= kpp[k1][p2][p1]
			// ミラー、書き出すのやめるほうがいいかも…よくわからない。
			= kpp[mk1][mp1][mp2]
			= kpp[mk1][mp2][mp1]
			= value;

	}

	// kpp_write()するときの一番若いアドレス(index)を返す。
	u64 get_kpp_index(Square k1, BonaPiece p1, BonaPiece p2)
	{
		BonaPiece mp1 = mir_piece[p1];
		BonaPiece mp2 = mir_piece[p2];
		Square  mk1 = Mir(k1);

		const auto q0 = &kpp[0][0][0];
		auto q1 = &kpp[k1][p1][p2] - q0;
		auto q2 = &kpp[k1][p2][p1] - q0;
		auto q3 = &kpp[mk1][mp1][mp2] - q0;
		auto q4 = &kpp[mk1][mp2][mp1] - q0;

		// ミラー位置も考慮したほうが良いのかはよくわからないが。
		return std::min({ q1, q2, q3, q4 });
	}

	// 学習時にkkpテーブルに値を書き出すためのヘルパ関数。
	// この関数を用いると、ミラー関係にある箇所などにも同じ値を書き込んでくれる。次元下げの一種。
	void kkp_write(Square k1, Square k2, BonaPiece p1, ValueKkp value)
	{
		// '~'をinv記号,'M'をmirror記号だとして
		//   [  k1 ][  k2 ][  p1 ]
		//   [ ~k2 ][ ~k1 ][ ~p1 ] (1)
		//   [M k1 ][M k2 ][M p1 ]
		//   [M~k2 ][M~k1 ][M~p1 ] (2)
		// は、同じ値であるべきなので、その4箇所に書き出す。
		// ただし、(1)[0],(2)[0]は[k1][k2][p1][0]とは符号が逆なので注意。

		BonaPiece ip1 = inv_piece[p1];
		BonaPiece mp1 = mir_piece[p1];
		BonaPiece mip1 = mir_piece[ip1];
		Square  mk1 = Mir(k1);
		Square  ik1 = Inv(k1);
		Square mik1 = Mir(ik1);
		Square  mk2 = Mir(k2);
		Square  ik2 = Inv(k2);
		Square mik2 = Mir(ik2);

		// ASSERT_LV3(kkp[k1][k2][p1] == kkp[mk1][mk2][mp1]);

		kkp[k1][k2][p1]
			= kkp[mk1][mk2][mp1]
			= value;

	// kkpに関して180度のflipは入れないほうが良いのでは..
#ifdef USE_KKP_FLIP_WRITE
		/*
		ASSERT_LV3(kkp[k1][k2][p1][0] == -kkp[ik2][ik1][ip1][0]);
		ASSERT_LV3(kkp[k1][k2][p1][1] == +kkp[ik2][ik1][ip1][1]);
		ASSERT_LV3(kkp[ik2][ik1][ip1] == kkp[mik2][mik1][mip1]);
		*/

		kkp[ik2][ik1][ip1][0]
			= kkp[mik2][mik1][mip1][0]
			= -value[0];

		kkp[ik2][ik1][ip1][1]
			= kkp[mik2][mik1][mip1][1]
			= +value[1];
#endif
	}

	// kkp_write()するときの一番若いアドレス(index)を返す。
	// flipはvalueの符号が変わるのでここでは考慮しない。
	u64 get_kkp_index(Square k1, Square k2 , BonaPiece p1)
	{
		Square  mk1 = Mir(k1);
		Square  mk2 = Mir(k2);
		BonaPiece mp1 = mir_piece[p1];

		const auto q0 = &kkp[0][0][0];
		auto q1 = &kkp[k1][k2][p1] - q0;
		auto q2 = &kkp[mk1][mk2][mp1] - q0;

		return std::min({ q1, q2 });
	}

	// --- デバッグ用出力。

	template <typename T>
	std::ostream& operator << (std::ostream& os, const std::array<T, 2>& p)
	{
		os << "{ " << p[0] << " , " << p[1] << " } ";
		return os;
	}

	// 勾配等の配列
	struct Weight
	{
		// mini-batch 1回分の勾配の累積値
		FloatPair g;

#if defined (USE_SGD_UPDATE)
		// SGDの更新式
		//   w = w - ηg

		// SGDの場合、勾配自動調整ではないので、損失関数に合わせて適宜調整する必要がある。

		static double eta;

		// この特徴の出現回数
		u32   count;
#endif

#if defined (USE_ADA_GRAD_UPDATE)
		// AdaGradの更新式
		//   v = v + g^2
		// ※　vベクトルの各要素に対して、gの各要素の2乗を加算するの意味
		//   w = w - ηg/sqrt(v)

		// 学習率η = 0.01として勾配が一定な場合、1万回でη×199ぐらい。
		// cf. [AdaGradのすすめ](http://qiita.com/ak11/items/7f63a1198c345a138150)
		// 初回更新量はeta。そこから小さくなっていく。
		static double eta;
		
		// AdaGradのg2
		FloatPair g2;
#endif


#if defined (USE_ADAM_UPDATE)
		// 普通のAdam
		// cf. http://qiita.com/skitaoka/items/e6afbe238cd69c899b2a

//		const LearnFloatType alpha = 0.001f;

		// const double eta = 32.0/64.0;

		static constexpr double beta = LearnFloatType(0.9);
		static constexpr double gamma = LearnFloatType(0.999);
		
		static constexpr double epsilon = LearnFloatType(10e-8);

		static constexpr double eta = LearnFloatType(32.0/64.0);

		FloatPair v;
		FloatPair r;

		// これはupdate()呼び出し前に計算して代入されるものとする。
		// bt = pow(β,epoch) , rt = pow(γ,epoch)
		static double bt;
		static double rt;

#endif

		void add_grad(FloatPair g_)
		{
			g = g + g_;

#if defined (USE_SGD_UPDATE)
			count++;
#endif
		}

		// 勾配gにもとづいて、wをupdateする。

		template <typename T>
		void update(array<T,2>& w)
		{
#if defined (USE_SGD_UPDATE)
			
			if (g[0] == 0 && g[1] == 0)
				return ;

			// 勾配はこの特徴の出現したサンプル数で割ったもの。
			if (count == 0)
				return ;

			// 今回の更新量
			for (int i = 0; i < 2; ++i)
				g[i] = (LearnFloatType)(eta * g[i] / (LearnFloatType)count);

			// あまり大きいと発散しかねないので移動量に制約を課す。
			SET_A_LIMIT_TO(g, -64.0f, 64.0f);

			for (int i = 0; i < 2; ++i)
				w[i] = T(w[i] - g[i]);

			count = 0;

#endif

#if defined ( USE_ADA_GRAD_UPDATE )

			// 普通のAdaGrad

			// g2[i] += g * g;
			// w[i] -= η * g / sqrt(g2[i] + epsilon);
			// g = 0 // mini-batchならこのタイミングで勾配配列クリア。
			
			constexpr double epsilon = 0.000001;

			for (int i = 0; i < 2; ++i)
			{
				g2[i] += g[i] * g[i];

				// 更新式の分母でepsilonを足すとは言え、
				// g2が小さいときはノイズが大きいので更新しないことにする。
				if (g2[i] < epsilon * 10)
					continue;

				w[i] -= (T)(eta * (double)g[i] / sqrt((double)g2[i] + epsilon));
			}
#endif

#ifdef USE_ADAM_UPDATE
			// Adamのときは勾配がゼロのときでもwの更新は行なう。

			// v = βv + (1-β)g
			// r = γr + (1-γ)g^2
			// w = w - α*v / (sqrt(r/(1-γ^t))+e) * (1-β^t)
			// rt = 1-γ^t , bt = 1-β^tとして、これは事前に計算しておくとすると、
			// w = w - α*v / (sqrt(r/rt) + e) * (bt)

			for (int i = 0; i < 2; ++i)
			{
				v[i] = LearnFloatType(beta * v[i] + (1.0 - beta) * g[i]);
				r[i] = LearnFloatType(gamma * r[i] + (1.0 - gamma) * (g[i]*g[i]));

				// sqrt()の中身がゼロになりうるので、1回目の割り算を先にしないとアンダーフローしてしまう。
				// 例) epsilon * bt = 0
				// あと、doubleでないと計算精度が足りなくて死亡する。
				w[i] = T(w[i] - LearnFloatType(eta / (sqrt((double)r[i] / rt) + epsilon) * v[i] / bt));
			}

#endif

			// 絶対値を抑制する。
			// KK,KKPもいまどきの手法だとs16に収まる。
			SET_A_LIMIT_TO(w, (T)((s32)INT16_MIN * 3 / 4), (T)((s32)INT16_MAX * 3 / 4));

			// この要素に関するmini-batchの1回分の更新が終わったのでgをクリア
			g = { 0 , 0 };
		}
	};

	// デバッグ用にgの値を出力する。
	std::ostream& operator<<(std::ostream& os, const Eval::Weight& w)
	{
		os << "g = " << w.g
#if defined ( USE_ADA_GRAD_UPDATE )
			<< ", g2 = " << w.g2;
#endif
			;
		return os;
	}


#if defined (USE_SGD_UPDATE) || defined(USE_ADA_GRAD_UPDATE)
	double Weight::eta;
#elif defined USE_ADAM_UPDATE
	// 1.0 - pow(beta,epoch)
	double Weight::bt;
	// 1.0 - pow(gamma,epoch)
	double Weight::rt;
#endif

	Weight(*kk_w_)[SQ_NB][SQ_NB];
	Weight(*kpp_w_)[SQ_NB][fe_end][fe_end];
	Weight(*kkp_w_)[SQ_NB][SQ_NB][fe_end];

#define kk_w (*kk_w_)
#define kpp_w (*kpp_w_)
#define kkp_w (*kkp_w_)

	// ユーザーがlearnコマンドで指定したetaの値。0.0fなら設定なしなのでdefault値を用いるべき。
	static float user_eta = 0.0f;

	// 学習のときの勾配配列の初期化
	void init_grad(float eta)
	{
		user_eta = eta;

		if (kk_w_ == nullptr)
		{
			size_t size;

			// KK
			size = size_t(SQ_NB)*size_t(SQ_NB);
			kk_w_ = (Weight(*)[SQ_NB][SQ_NB])new Weight[size];
			memset(kk_w_, 0, sizeof(Weight) * size);
#ifdef RESET_TO_ZERO_VECTOR
			cout << "[RESET_TO_ZERO_VECTOR]";
			memset(kk_, 0, sizeof(ValueKk) * size);
#endif

			// KKP
			size = size_t(SQ_NB)*size_t(SQ_NB)*size_t(fe_end);
			kkp_w_ = (Weight(*)[SQ_NB][SQ_NB][fe_end])new Weight[size];
			memset(kkp_w_, 0, sizeof(Weight) * size);
#ifdef RESET_TO_ZERO_VECTOR
			memset(kkp_, 0, sizeof(ValueKkp) * size);
#endif

			// KPP
			size = size_t(SQ_NB)*size_t(fe_end)*size_t(fe_end);
			kpp_w_ = (Weight(*)[SQ_NB][fe_end][fe_end])new Weight[size];
			memset(kpp_w_, 0, sizeof(Weight) * size);
#ifdef RESET_TO_ZERO_VECTOR
			memset(kpp_, 0, sizeof(ValueKpp) * size);
#endif

		}
	}

	// 現在の局面で出現している特徴すべてに対して、勾配値を勾配配列に加算する。
	void add_grad(Position& pos, Color rootColor, double delta_grad)
	{
		// Aperyに合わせておく。
		delta_grad /= 32.0 /*FV_SCALE*/;

		// 勾配
		FloatPair g = {
			// 手番を考慮しない値
			(rootColor == BLACK) ? LearnFloatType(delta_grad) : -LearnFloatType(delta_grad),

			// 手番を考慮する値
			(rootColor == pos.side_to_move()) ? LearnFloatType(delta_grad) : -LearnFloatType(delta_grad)
		};

		// 180度盤面を回転させた位置関係に対する勾配
		FloatPair g_flip = { -g[0] , +g[1] };

		Square sq_bk = pos.king_square(BLACK);
		Square sq_wk = pos.king_square(WHITE);

		auto& pos_ = *const_cast<Position*>(&pos);

		auto list_fb = pos_.eval_list()->piece_list_fb();
		auto list_fw = pos_.eval_list()->piece_list_fw();

		// KK

		// KKはmirrorとflipするのは微妙。
		// 後手振り飛車はすごく不利だとかそういうのを学習させたいので..。

		kk_w[sq_bk][sq_wk].add_grad(g);
		// flipした位置関係にも書き込む
		//kk_w[Inv(sq_wk)][Inv(sq_bk)].add_grad(g_flip);
		
		for (int i = 0; i < PIECE_NO_KING; ++i)
		{
			BonaPiece k0 = list_fb[i];
			BonaPiece k1 = list_fw[i];

			// このループではk0 == l0は出現しない。
			// それはKPであり、KKPの計算に含まれると考えられるから。
			for (int j = 0; j < i; ++j)
			{
				BonaPiece l0 = list_fb[j];
				BonaPiece l1 = list_fw[j];

				// KPP

				// kpp配列に関してはミラー(左右判定)とフリップ(180度回転)の次元下げを行う。

				// l0 == k0ときは2度加算してはならないので、この処理は省略するべきだが、
				// …が、kppはkpは含まないことになっているので、気にしなくて良い。

				// KPPの手番ありのとき
				((Weight*)kpp_w_)[get_kpp_index(sq_bk, k0, l0)].add_grad(g);
				((Weight*)kpp_w_)[get_kpp_index(Inv(sq_wk), k1, l1)].add_grad(g_flip);

			}

			// KKP

			// kkpは次元下げ、ミラーも含めてやらないほうがいいかも。どうせ教師の数は足りているし、
			// 右と左とでは居飛車、振り飛車的な戦型選択を暗に含むからミラーするのが良いとは限らない。
			// 180度回転も先手後手とは非対称である可能性があるのでここのフリップは善悪微妙。
			
			((Weight*)kkp_w_)[get_kkp_index(sq_bk,sq_wk,k0)].add_grad(g);
		//	((Weight*)kkp_w_)[get_kkp_index(Inv(sq_wk), Inv(sq_bk), k1)].add_grad(g_flip);
		}

	}


	// 現在の勾配をもとにSGDかAdaGradか何かする。
	void update_weights(u64 mini_batch_size , u64 epoch)
	{

		//
		// 学習メソッドに応じた学習率設定
		//

		// SGD
#if defined (USE_SGD_UPDATE)
		
#if defined (LOSS_FUNCTION_IS_CROSS_ENTOROPY)

#ifdef USE_SGD_UPDATE
		Weight::eta = 3.2f;

		//Weight::eta = 100.0f;
#endif

#elif defined (LOSS_FUNCTION_IS_WINNING_PERCENTAGE)

#ifdef USE_SGD_UPDATE
//		Weight::eta = 150.0f;

		Weight::eta = 32.0f;
#endif

		// ユーザーがlearnコマンドでetaの値を指定していたら。
		if (user_eta != 0)
			Weight::eta = user_eta;

#endif

		// AdaGrad
#elif defined USE_ADA_GRAD_UPDATE

		// この係数、mini-batch sizeの影響を受けるのどうかと思うが、AdaGradのようなものなら問題ない。
		// デフォルトでは30.0
		Weight::eta = 30.0f;

		// ユーザーがlearnコマンドでetaの値を指定していたらその数値を反映。
		if (user_eta != 0)
			Weight::eta = user_eta;


		// Adam
#elif defined USE_ADAM_UPDATE

		Weight::bt = 1.0 - pow((double)Weight::beta , (double)epoch);
		Weight::rt = 1.0 - pow((double)Weight::gamma, (double)epoch);

#endif


		{
// 学習をopenmpで並列化(この間も局面生成は続くがまあ、問題ないやろ..
#ifdef _OPENMP
#pragma omp parallel for schedule (static)
#endif
			// Open MP対応のため、int型の変数を使う必要がある。(悲しい)
			for (int k1 = SQ_ZERO; k1 < SQ_NB; ++k1)
				for (auto k2 : SQ)
				{
					kk_w[k1][k2].update(kk[k1][k2]);
				}

#ifdef _OPENMP
#pragma omp parallel for schedule (static)
#endif
			for (int p1 = BONA_PIECE_ZERO; p1 < fe_end; ++p1)
			{
				for (auto k : SQ)
				{
					// p1とp2を入れ替えたものは、kpp_write()で書き込まれるはずなので無視して良い。
					// また、p1 == p2はKPであり、これはKKPの計算のときにやっているので無視して良い。
					// kpp[k][p1][p2]==0であるものとする。念のためにクリアしておく。
					kpp[k][p1][p1] = { 0,0 };
					
					for (int p2 = p1 + 1; p2 < fe_end; ++p2)
					{
						// (次元下げとして)一番若いアドレスにgradの値が書き込まれているから、
						// この値に基いて以下のvを更新すれば良い。
						auto& w = ((Weight*)kpp_w_)[get_kpp_index(k, (BonaPiece)p1, (BonaPiece)p2)];
						auto& v = kpp[k][p1][p2];

						w.update(v);
						kpp_write(k, (BonaPiece)p1, (BonaPiece)p2, v);
					}
				}
			}

			// 外側のループをk1にすると、ループ回数が81になって、40HTのときに1余るのが嫌。
			// ゆえに外側のループはpに変更する。
#ifdef _OPENMP
#pragma omp parallel for schedule (static)
#endif
			for (int p = BONA_PIECE_ZERO; p < fe_end; ++p)
			{
				for (auto k1 : SQ)
					for (auto k2 : SQ)
					{
						auto& w = kkp_w[k1][k2][p];
						auto& v = kkp[k1][k2][p];

						w.update(v);

						kkp_write(k1, k2, (BonaPiece)p, v);
					}
			}
		}

	}


	// 学習のためのテーブルの初期化
	void eval_learn_init()
	{
		// fとeとの交換
		int t[] = {
			f_hand_pawn - 1    , e_hand_pawn - 1   ,
			f_hand_lance - 1   , e_hand_lance - 1  ,
			f_hand_knight - 1  , e_hand_knight - 1 ,
			f_hand_silver - 1  , e_hand_silver - 1 ,
			f_hand_gold - 1    , e_hand_gold - 1   ,
			f_hand_bishop - 1  , e_hand_bishop - 1 ,
			f_hand_rook - 1    , e_hand_rook - 1   ,
			f_pawn             , e_pawn            ,
			f_lance            , e_lance           ,
			f_knight           , e_knight          ,
			f_silver           , e_silver          ,
			f_gold             , e_gold            ,
			f_bishop           , e_bishop          ,
			f_horse            , e_horse           ,
			f_rook             , e_rook            ,
			f_dragon           , e_dragon          ,
		};

		// 未初期化の値を突っ込んでおく。
		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
		{
			inv_piece[p] = BONA_PIECE_NOT_INIT;

			// mirrorは手駒に対しては機能しない。元の値を返すだけ。
			mir_piece[p] = (p < f_pawn) ? p : BONA_PIECE_NOT_INIT;
		}

		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
		{
			for (int i = 0; i < 32 /* t.size() */; i += 2)
			{
				if (t[i] <= p && p < t[i + 1])
				{
					Square sq = (Square)(p - t[i]);

					// 見つかった!!
					BonaPiece q = (p < fe_hand_end) ? BonaPiece(sq + t[i + 1]) : (BonaPiece)(Inv(sq) + t[i + 1]);
					inv_piece[p] = q;
					inv_piece[q] = p;

					/*
					ちょっとトリッキーだが、pに関して盤上の駒は
					p >= fe_hand_end
					のとき。

					このpに対して、nを整数として(上のコードのiは偶数しかとらない)、
					a)  t[2n + 0] <= p < t[2n + 1] のときは先手の駒
					b)  t[2n + 1] <= p < t[2n + 2] のときは後手の駒
					　である。

					 ゆえに、a)の範囲にあるpをq = Inv(p-t[2n+0]) + t[2n+1] とすると180度回転させた升にある後手の駒となる。
					 そこでpとqをswapさせてinv_piece[ ]を初期化してある。
					 */

					 // 手駒に関してはmirrorなど存在しない。
					if (p < fe_hand_end)
						continue;

					BonaPiece r1 = (BonaPiece)(Mir(sq) + t[i]);
					mir_piece[p] = r1;
					mir_piece[r1] = p;

					BonaPiece p2 = (BonaPiece)(sq + t[i + 1]);
					BonaPiece r2 = (BonaPiece)(Mir(sq) + t[i + 1]);
					mir_piece[p2] = r2;
					mir_piece[r2] = p2;

					break;
				}
			}
		}

		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
			if (inv_piece[p] == BONA_PIECE_NOT_INIT
				|| mir_piece[p] == BONA_PIECE_NOT_INIT
				)
			{
				// 未初期化のままになっている。上のテーブルの初期化コードがおかしい。
				ASSERT(false);
			}

#if 0
		// 評価関数のミラーをしても大丈夫であるかの事前検証
		// 値を書き込んだときにassertionがあるので、ミラーしてダメである場合、
		// そのassertに引っかかるはず。

		// AperyのWCSC26の評価関数、kppのp1==0とかp1==20(後手の0枚目の歩)とかの
		// ところにゴミが入っていて、これを回避しないとassertに引っかかる。

		std::unordered_set<BonaPiece> s;
		vector<int> a = {
			f_hand_pawn - 1,e_hand_pawn - 1,
			f_hand_lance - 1, e_hand_lance - 1,
			f_hand_knight - 1, e_hand_knight - 1,
			f_hand_silver - 1, e_hand_silver - 1,
			f_hand_gold - 1, e_hand_gold - 1,
			f_hand_bishop - 1, e_hand_bishop - 1,
			f_hand_rook - 1, e_hand_rook - 1,
		};
		for (auto b : a)
			s.insert((BonaPiece)b);

		// さらに出現しない升の盤上の歩、香、桂も除外(Aperyはここにもゴミが入っている)
		for (Rank r = RANK_1; r <= RANK_2; ++r)
			for (File f = FILE_1; f <= FILE_9; ++f)
			{
				if (r == RANK_1)
				{
					// 1段目の歩
					BonaPiece b1 = BonaPiece(f_pawn + (f | r));
					s.insert(b1);
					s.insert(inv_piece[b1]);

					// 1段目の香
					BonaPiece b2 = BonaPiece(f_lance + (f | r));
					s.insert(b2);
					s.insert(inv_piece[b2]);
				}

				// 1,2段目の桂
				BonaPiece b = BonaPiece(f_knight + (f | r));
				s.insert(b);
				s.insert(inv_piece[b]);
			}

		cout << "\nchecking kpp_write()..";
		for (auto sq : SQ)
		{
			cout << sq << ' ';
			for (BonaPiece p1 = BONA_PIECE_ZERO; p1 < fe_end; ++p1)
				for (BonaPiece p2 = BONA_PIECE_ZERO; p2 < fe_end; ++p2)
					if (!s.count(p1) && !s.count(p2))
						kpp_write(sq, p1, p2, kpp[sq][p1][p2]);
		}
		cout << "\nchecking kkp_write()..";

		for (auto sq1 : SQ)
		{
			cout << sq1 << ' ';
			for (auto sq2 : SQ)
				for (BonaPiece p1 = BONA_PIECE_ZERO; p1 < fe_end; ++p1)
					if (!s.count(p1))
						kkp_write(sq1, sq2, p1, kkp[sq1][sq2][p1]);
		}
		cout << "..done!" << endl;
#endif
	}


	void save_eval(std::string dir_name)
	{
		{
			auto eval_dir = path_combine((string)Options["EvalSaveDir"], dir_name);

			cout << "save_eval() start. folder = " << eval_dir << endl;

			// すでにこのフォルダがあるならmkdir()に失敗するが、
			// 別にそれは構わない。なければ作って欲しいだけ。
			// また、EvalSaveDirまでのフォルダは掘ってあるものとする。
			
			MKDIR(eval_dir);

			// KK
			std::ofstream ofsKK(path_combine(eval_dir , KK_BIN) , std::ios::binary);
			if (!ofsKK.write(reinterpret_cast<char*>(kk), sizeof(kk)))
				goto Error;

			// KKP
			std::ofstream ofsKKP(path_combine(eval_dir , KKP_BIN) , std::ios::binary);
			if (!ofsKKP.write(reinterpret_cast<char*>(kkp), sizeof(kkp)))
				goto Error;

			// KPP
			std::ofstream ofsKPP(path_combine(eval_dir , KPP_BIN) , std::ios::binary);
			if (!ofsKPP.write(reinterpret_cast<char*>(kpp), sizeof(kpp)))
				goto Error;

			cout << "save_eval() finished. folder = " << eval_dir <<  endl;

			return;
		}

	Error:;
		cout << "Error : save_eval() failed" << endl;
	}

} // namespace Eval

#endif // EVAL_LEARN

#endif
